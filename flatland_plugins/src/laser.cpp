/*
 *  ______                   __  __              __
 * /\  _  \           __    /\ \/\ \            /\ \__
 * \ \ \L\ \  __  __ /\_\   \_\ \ \ \____    ___\ \ ,_\   ____
 *  \ \  __ \/\ \/\ \\/\ \  /'_` \ \ '__`\  / __`\ \ \/  /',__\
 *   \ \ \/\ \ \ \_/ |\ \ \/\ \L\ \ \ \L\ \/\ \L\ \ \ \_/\__, `\
 *    \ \_\ \_\ \___/  \ \_\ \___,_\ \_,__/\ \____/\ \__\/\____/
 *     \/_/\/_/\/__/    \/_/\/__,_ /\/___/  \/___/  \/__/\/___/
 * @copyright Copyright 2017 Avidbots Corp.
 * @name	  laser.cpp
 * @brief   Laser plugin
 * @author  Chunshang Li
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Avidbots Corp.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Avidbots Corp. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include <flatland_plugins/laser.h>
#include <flatland_server/collision_filter_registry.h>
#include <flatland_server/exceptions.h>
#include <flatland_server/model_plugin.h>
#include <flatland_server/yaml_reader.h>
#include <geometry_msgs/TransformStamped.h>
#include <pluginlib/class_list_macros.h>
#include <boost/algorithm/string/join.hpp>
#include <cmath>
#include <limits>
#include <chrono>

using namespace flatland_server;

namespace flatland_plugins {

void Laser::OnInitialize(const YAML::Node& config) {
  ParseParameters(config);

  update_timer_.SetRate(update_rate_);
  scan_publisher_ = nh_.advertise<sensor_msgs::LaserScan>(topic_, 1);

  // construct the body to laser transformation matrix once since it never
  // changes
  double c = cos(origin_.theta);
  double s = sin(origin_.theta);
  double x = origin_.x, y = origin_.y;
  m_body_to_laser_ << c, -s, x, s, c, y, 0, 0, 1;

  unsigned int num_laser_points =
      std::lround((max_angle_ - min_angle_) / increment_) + 1;

  // initialize size for the matrix storing the laser points
  m_laser_points_ = Eigen::MatrixXf(3, num_laser_points);
  m_world_laser_points_ = Eigen::MatrixXf(3, num_laser_points);
  m_lastMaxFractions_ = std::vector<float>(num_laser_points, 1.0);
  v_zero_point_ << 0, 0, 1;

  // pre-calculate the laser points w.r.t to the laser frame, since this never
  // changes
  for (unsigned int i = 0; i < num_laser_points; i++) {
    
    float angle = min_angle_ + i * increment_;
    if (upside_down_) {  // Laser inverted, so laser local frame angles are also inverted
      angle = -angle;
    }

    float x = range_ * cos(angle);
    float y = range_ * sin(angle);

    m_laser_points_(0, i) = x;
    m_laser_points_(1, i) = y;
    m_laser_points_(2, i) = 1;
  }

  // initialize constants in the laser scan message
  laser_scan_.angle_min = min_angle_;
  laser_scan_.angle_max = max_angle_;
  laser_scan_.angle_increment = increment_;
  laser_scan_.time_increment = 0;
  laser_scan_.scan_time = 0;
  laser_scan_.range_min = 0;
  laser_scan_.range_max = range_;
  laser_scan_.ranges.resize(num_laser_points);
  if (reflectance_layers_bits_)
    laser_scan_.intensities.resize(num_laser_points);
  else
    laser_scan_.intensities.resize(0);
  laser_scan_.header.seq = 0;
  laser_scan_.header.frame_id =
      tf::resolve("", GetModel()->NameSpaceTF(frame_id_));

  // Broadcast transform between the body and laser
  tf::Quaternion q;
  if (upside_down_) {
    q.setRPY(M_PI, 0, origin_.theta);
  } else {
    q.setRPY(0, 0, origin_.theta);
  }
  

  laser_tf_.header.frame_id = tf::resolve(
      "", GetModel()->NameSpaceTF(body_->GetName()));  // Todo: parent_tf param
  laser_tf_.child_frame_id =
      tf::resolve("", GetModel()->NameSpaceTF(frame_id_));
  laser_tf_.transform.translation.x = origin_.x;
  laser_tf_.transform.translation.y = origin_.y;
  laser_tf_.transform.translation.z = 0;
  laser_tf_.transform.rotation.x = q.x();
  laser_tf_.transform.rotation.y = q.y();
  laser_tf_.transform.rotation.z = q.z();
  laser_tf_.transform.rotation.w = q.w();
}

void Laser::BeforePhysicsStep(const Timekeeper& timekeeper) {
  // keep the update rate
  if (!update_timer_.CheckUpdate(timekeeper)) {
    return;
  }

  // only compute and publish when the number of subscribers is not zero, or always_publish_ is true
  if (always_publish_ || scan_publisher_.getNumSubscribers() > 0) {
    // START_PROFILE(timekeeper, "compute laser range");
    auto start = std::chrono::steady_clock::now();
    ComputeLaserRanges();

    ROS_INFO_THROTTLE_NAMED(
        1, "Laser Plugin", "took %luus",
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());

    // END_PROFILE(timekeeper, "compute laser range");
    laser_scan_.header.stamp = timekeeper.GetSimTime();
    scan_publisher_.publish(laser_scan_);
    publications_++;
  }

  if (broadcast_tf_) {
    laser_tf_.header.stamp = timekeeper.GetSimTime();
    tf_broadcaster_.sendTransform(laser_tf_);
  }
}

void Laser::ComputeLaserRanges() {
  // get the transformation matrix from the world to the body, and get the
  // world to laser frame transformation matrix by multiplying the world to body
  // and body to laser
  const b2Transform& t = body_->GetPhysicsBody()->GetTransform();
  m_world_to_body_ << t.q.c, -t.q.s, t.p.x, t.q.s, t.q.c, t.p.y, 0, 0, 1;
  m_world_to_laser_ = m_world_to_body_ * m_body_to_laser_;

  // Get the laser points in the world frame by multiplying the laser points in
  // the laser frame to the transformation matrix from world to laser frame
  m_world_laser_points_ = m_world_to_laser_ * m_laser_points_;
  // Get the (0, 0) point in the laser frame
  v_world_laser_origin_ = m_world_to_laser_ * v_zero_point_;

  // Conver to Box2D data types
  b2Vec2 laser_origin_point(v_world_laser_origin_(0), v_world_laser_origin_(1));

  // Results vector : clustered to avoid too many locks
  constexpr size_t clusterSize = 10;
  // The last cluster may have a different size
  size_t lastClusterSize = laser_scan_.ranges.size() % clusterSize;
  size_t nbCluster =
      laser_scan_.ranges.size() / clusterSize + (lastClusterSize == 0 ? 0 : 1);
  if (lastClusterSize == 0) {
    lastClusterSize = clusterSize;
  }
  std::vector<std::future<std::vector<std::pair<float, float>>>> results(
      nbCluster);

  // At scan n    : we hit an obstacle at distance range_ * coef
  // At scan n+1  : we try a first scan with length = range_ * coef *
  // lastScanExpansion
  // lastScanExpansion makes it possible to take into account of the motion
  // and the rotation of the laser
  const float lastScanExpansion =
      1.0f + std::max<float>(4.0f / (range_ * update_rate_), 0.05f);

  // loop through the laser points and call the Box2D world raycast
  // by enqueueing the callback
  for (unsigned int i = 0; i < nbCluster; ++i) {
    size_t currentClusterSize =
        (i == nbCluster - 1 ? lastClusterSize : clusterSize);
    results[i] = pool_.enqueue([i, currentClusterSize, clusterSize, this,
                                laser_origin_point, lastScanExpansion] {
      std::vector<std::pair<float, float>> out(currentClusterSize);
      size_t currentIndice = i * clusterSize;
      // Iterating over the currentClusterSize indices, starting at i *
      // clusterSize
      for (size_t j = 0; j < currentClusterSize; ++j, ++currentIndice) {
        b2Vec2 laser_point;
        laser_point.x = m_world_laser_points_(0, currentIndice);
        laser_point.y = m_world_laser_points_(1, currentIndice);
        LaserCallback cb(this);

        // 1 - Take advantage of the last scan : we may hit the same obstacle
        // (if any),
        // with the first part of the ray being [0 fraction] * range_
        float fraction = std::min<float>(
            lastScanExpansion * m_lastMaxFractions_[currentIndice], 1.0f);
        GetModel()->GetPhysicsWorld()->RayCast(&cb, laser_origin_point,
                                               laser_point, fraction);

        // 2 - If no hit detected, we relaunch the scan
        // with the second part of the ray being [fraction 1] * range_
        if (!cb.did_hit_ && fraction < 1.0f) {
          b2Vec2 new_origin_point =
              fraction * laser_point + (1 - fraction) * laser_origin_point;
          GetModel()->GetPhysicsWorld()->RayCast(&cb, new_origin_point,
                                                 laser_point, 1.0f);
          if (cb.did_hit_)
            cb.fraction_ = fraction + cb.fraction_ - cb.fraction_ * fraction;
        }

        // 3 - Let's check the result
        if (cb.did_hit_) {
          m_lastMaxFractions_[currentIndice] = cb.fraction_;
          out[j] = std::make_pair<float, float>(
              cb.fraction_ * this->range_ + this->noise_gen_(this->rng_),
              static_cast<float>(cb.intensity_));
        } else {
          m_lastMaxFractions_[currentIndice] = cb.fraction_;
          out[j] = std::make_pair<float, float>(NAN, 0);
        }
      }
      return out;
    });
  }

  // Unqueue all of the future'd results
  const auto reflectance = reflectance_layers_bits_;
  auto i = laser_scan_.intensities.begin();
  auto r = laser_scan_.ranges.begin();
  for (auto clusterIte = results.begin(); clusterIte != results.end();
        ++clusterIte) {
    auto resultCluster = clusterIte->get();
    for (auto ite = resultCluster.begin(); ite != resultCluster.end();
          ++ite, ++i, ++r) {
      // Loop unswitching should occur
      if (reflectance) {
        *i = ite->second;
        *r = ite->first;
      } else
        *r = ite->first;
    }
  }
}

float LaserCallback::ReportFixture(b2Fixture* fixture, const b2Vec2& point,
                                   const b2Vec2& normal, float fraction) {
  uint16_t category_bits = fixture->GetFilterData().categoryBits;
  // only register hit in the specified layers
  if (!(category_bits & parent_->layers_bits_)) {
    return -1.0f;  // return -1 to ignore this hit
  }

  // Don't return on hitting sensors... they're not real
  if (fixture->IsSensor()) return -1.0f;

  if (category_bits & parent_->reflectance_layers_bits_) {
    intensity_ = 255.0;
  }

  did_hit_ = true;
  fraction_ = fraction;

  return fraction;
}

void Laser::ParseParameters(const YAML::Node& config) {
  YamlReader reader(config);
  std::string body_name = reader.Get<std::string>("body");
  topic_ = reader.Get<std::string>("topic", "scan");
  frame_id_ = reader.Get<std::string>("frame", GetName());
  broadcast_tf_ = reader.Get<bool>("broadcast_tf", true);
  always_publish_ = reader.Get<bool>("always_publish", false);
  update_rate_ = reader.Get<double>("update_rate",
                                    std::numeric_limits<double>::infinity());
  origin_ = reader.GetPose("origin", Pose(0, 0, 0));
  range_ = reader.Get<double>("range");
  noise_std_dev_ = reader.Get<double>("noise_std_dev", 0);

  upside_down_ = reader.Get<bool>("upside_down", false);

  std::vector<std::string> layers =
      reader.GetList<std::string>("layers", {"all"}, -1, -1);

  YamlReader angle_reader = reader.Subnode("angle", YamlReader::MAP);
  min_angle_ = angle_reader.Get<double>("min");
  max_angle_ = angle_reader.Get<double>("max");
  increment_ = angle_reader.Get<double>("increment");

  angle_reader.EnsureAccessedAllKeys();
  reader.EnsureAccessedAllKeys();

  if (increment_ < 0) {
    if (min_angle_ < max_angle_) {
      throw YAMLException(
          "Invalid \"angle\" params, must have min > max when increment < 0");
    }

  } else if (increment_ > 0) {
    if (max_angle_ < min_angle_) {
      throw YAMLException(
          "Invalid \"angle\" params, must have max > min when increment > 0");
    }
  } else {
    throw YAMLException(
        "Invalid \"angle\" params, increment must not be zero!");
  }

  body_ = GetModel()->GetBody(body_name);
  if (!body_) {
    throw YAMLException("Cannot find body with name " + body_name);
  }

  std::vector<std::string> invalid_layers;
  layers_bits_ = GetModel()->GetCfr()->GetCategoryBits(layers, &invalid_layers);
  if (!invalid_layers.empty()) {
    throw YAMLException("Cannot find layer(s): {" +
                        boost::algorithm::join(invalid_layers, ",") + "}");
  }

  std::vector<std::string> reflectance_layer = {"reflectance"};
  reflectance_layers_bits_ =
      GetModel()->GetCfr()->GetCategoryBits(reflectance_layer, &invalid_layers);

  // init the random number generators
  std::random_device rd;
  rng_ = std::default_random_engine(rd());
  noise_gen_ = std::normal_distribution<float>(0.0, noise_std_dev_);

  ROS_INFO(  //"LaserPlugin",
      "Laser %s params: topic(%s) body(%s, %p) origin(%f,%f,%f) upside_down(%d)"
      "frame_id(%s) broadcast_tf(%d) update_rate(%f) range(%f)  "
      "noise_std_dev(%f) angle_min(%f) angle_max(%f) "
      "angle_increment(%f) layers(0x%u {%s}) always_publish(%d)",
      GetName().c_str(), topic_.c_str(), body_name.c_str(), body_, origin_.x,
      origin_.y, origin_.theta, upside_down_, frame_id_.c_str(), broadcast_tf_,
      update_rate_, range_, noise_std_dev_, min_angle_, max_angle_, increment_,
      layers_bits_, boost::algorithm::join(layers, ",").c_str(), always_publish_);
}
};  // namespace flatland_plugins

PLUGINLIB_EXPORT_CLASS(flatland_plugins::Laser, flatland_server::ModelPlugin)
