/*
* Copyright (C) 2015, 2016 Jannik Buckelo <jannikbu@cs.uni-bremen.de>,
* Georg Bartels <georg.bartels@cs.uni-bremen.de>
*
*
* This file is part of giskard_examples.
*
* giskard_examples is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2 
* of the License, or (at your option) any later version.  
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License 
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <ros/ros.h>
#include <ros/package.h>
#include <sensor_msgs/JointState.h>
#include <std_msgs/Float64.h>
#include <std_msgs/UInt64.h>
#include <giskard_msgs/WholeBodyCommand.h>
#include <giskard_msgs/ControllerFeedback.h>
#include <yaml-cpp/yaml.h>
#include <giskard/giskard.hpp>
#include <kdl_conversions/kdl_msg.h>
#include <boost/lexical_cast.hpp>
#include <giskard_examples/utils.hpp>
#include <giskard_examples/watchdog.hpp>

// TODO: separate this into a library and executable part
// TODO: refactor this into classes

int nWSR_;
giskard::QPController controller_;
std::vector<std::string> joint_names_, double_names_;
std::vector<ros::Publisher> vel_controllers_;
ros::Publisher feedback_pub_, current_command_hash_pub_, current_command_pub_;
ros::Subscriber js_sub_;
Eigen::VectorXd state_;
bool controller_started_;
std::string frame_id_;
giskard_msgs::ControllerFeedback feedback_;
giskard_msgs::WholeBodyCommand current_command_;
size_t current_command_hash_hash_;
giskard_examples::Watchdog<ros::Time, ros::Duration> watchdog_;

void js_callback(const sensor_msgs::JointState::ConstPtr& msg)
{
  // TODO: turn this into a map!
  // is there a more efficient way?
  for (unsigned int i=0; i < joint_names_.size(); i++)
  {
    for (unsigned int j=0; j < msg->name.size(); j++)
    {
      if (msg->name[j].compare(joint_names_[i]) == 0)
      {
        state_[i] = msg->position[j];
      }
    }
  }

  if (!controller_started_)
    return;

  feedback_.header.stamp = msg->header.stamp;
  feedback_.header.frame_id = frame_id_;
  if (watchdog_.barking(msg->header.stamp))
  {
    // switch controller, and inform the outside world
    // TODO: replace me with something meaningful, once we have joint-control
    giskard_msgs::WholeBodyCommand watchdog_cmd;
    size_t watchdog_hash = giskard_examples::calculateHash<giskard_msgs::WholeBodyCommand>(watchdog_cmd);
 
    if (current_command_hash_hash_ != watchdog_hash)
    {
      current_command_hash_hash_ = watchdog_hash;
      current_command_pub_.publish(watchdog_cmd);
      std_msgs::UInt64 hash_msg;
      hash_msg.data = watchdog_hash;
      current_command_hash_pub_.publish(hash_msg);
    }
    for (unsigned int i=0; i < vel_controllers_.size(); i++)
    {
      std_msgs::Float64 command;
      command.data = 0.0;
      vel_controllers_[i].publish(command);
    }
  
//    for(size_t i=0; i<feedback_.commands.size(); ++i)
//      feedback_.commands[i].value = 0.0;
//    for(size_t i=0; i<feedback_.slacks.size(); ++i)
//      feedback_.slacks[i].value = 0.0;
//    feedback_pub_.publish(feedback_);
  }
  else
    if (controller_.update(state_, nWSR_))
    {
      for (unsigned int i=0; i < vel_controllers_.size(); i++)
      {
        std_msgs::Float64 command;
        command.data = controller_.get_command()[i];
        vel_controllers_[i].publish(command);
      }
   
      for(size_t i=0; i<feedback_.commands.size(); ++i)
        feedback_.commands[i].value = controller_.get_command()[i];
      for(size_t i=0; i<feedback_.slacks.size(); ++i)
        feedback_.slacks[i].value = controller_.get_slack()[i];
      for(size_t i=0; i<feedback_.doubles.size(); ++i)
        try
        {
          feedback_.doubles[i].value = controller_.get_scope().find_double_expression(feedback_.doubles[i].semantics)->value();
        }
        catch (const std::runtime_error& e)
        {
          ROS_WARN("Could not find internal double expression '%s'.", feedback_.doubles[i].semantics.c_str());
        }

      feedback_pub_.publish(feedback_);
    }
    else
    {
      ROS_WARN("Update failed.");
      ROS_DEBUG_STREAM("Update failed. State: " << state_);
    }

  // TODO: publish diagnostics
}

void print_eigen(const Eigen::VectorXd& command)
{
  std::string cmd_str = " ";
  for(size_t i=0; i<command.rows(); ++i)
    cmd_str += boost::lexical_cast<std::string>(command[i]) + " ";
  ROS_DEBUG("Command: (%s)", cmd_str.c_str());
}

Eigen::Matrix<double, 6, 1> process_pose(const geometry_msgs::PoseStamped& pose, 
    const std::string& frame_id, const std::string& bodypart)
{
  Eigen::Matrix<double, 6, 1> result;

  if(pose.header.frame_id.compare(frame_id_) != 0)
    throw std::runtime_error("frame_id of " + bodypart +
        "goal did not match expected '" + frame_id +"'. Ignoring goal");

  result[0] = pose.pose.position.x;
  result[1] = pose.pose.position.y;
  result[2] = pose.pose.position.z;

  KDL::Rotation rot;
  tf::quaternionMsgToKDL(pose.pose.orientation, rot);
  rot.GetEulerZYX(result[3], result[4], result[5]);

  return result;
}

void goal_callback(const giskard_msgs::WholeBodyCommand::ConstPtr& msg)
{
  size_t new_command_hash = giskard_examples::calculateHash<giskard_msgs::WholeBodyCommand>(*msg);
  if(current_command_hash_hash_ == new_command_hash)
  {
    watchdog_.kick(ros::Time::now());
    return;
  }

  try
  {
    if(msg->left_ee.process)
      state_.block<6,1>(joint_names_.size(), 0) = process_pose(msg->left_ee.goal, frame_id_, "left EE");
    if(msg->right_ee.process)
      state_.block<6,1>(joint_names_.size() + 6, 0) = process_pose(msg->right_ee.goal, frame_id_, "right EE");
  }
  catch(const std::exception& e)
  {
    ROS_WARN("%s", e.what());
    return;
  }

  current_command_hash_hash_ = new_command_hash;
  watchdog_.kick(ros::Time::now());

  if(msg->left_ee.process)
    current_command_.left_ee = msg->left_ee;
  if(msg->right_ee.process)
    current_command_.right_ee = msg->right_ee;
  current_command_pub_.publish(current_command_);

  std_msgs::UInt64 hash_msg;
  hash_msg.data = new_command_hash;
  current_command_hash_pub_.publish(hash_msg);

  // TODO: check that joint-state contains all necessary joints

  if (!controller_started_)
  {
    if (controller_.start(state_, nWSR_))
    {
      ROS_DEBUG("Controller started.");
      controller_started_ = true;
    }
    else
    {
      ROS_ERROR("Couldn't start controller.");
      print_eigen(state_);
    }
  }
}

giskard_msgs::ControllerFeedback initFeedbackMsg(const giskard::QPController& controller)
{
  giskard_msgs::ControllerFeedback msg;

  msg.commands.resize(controller.get_controllable_names().size());
  for(size_t i=0; i<controller.get_controllable_names().size(); ++i)
    msg.commands[i].semantics = controller.get_controllable_names()[i];

  msg.slacks.resize(controller.get_soft_constraint_names().size());
  for(size_t i=0; i<controller.get_soft_constraint_names().size(); ++i)
    msg.slacks[i].semantics = controller.get_soft_constraint_names()[i];

  msg.doubles.resize(double_names_.size());
  for(size_t i=0; i<double_names_.size(); ++i)
    msg.doubles[i].semantics = double_names_[i];

  return msg;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "whole_body_controller");
  ros::NodeHandle nh("~");

  nh.param("nWSR", nWSR_, 10);

  std::string controller_description;
  if (!nh.getParam("controller_description", controller_description))
  {
    ROS_ERROR("Parameter 'controller_description' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  // TODO: extract joint_names from controller description
  if (!nh.getParam("joint_names", joint_names_))
  {
    ROS_ERROR("Parameter 'joint_names' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  nh.getParam("internals/doubles", double_names_);

  if (!nh.getParam("frame_id", frame_id_))
  {
    ROS_ERROR("Parameter 'frame_id' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  double watchdog_period;
  if (!nh.getParam("watchdog_period", watchdog_period))
  {
    ROS_ERROR("Parameter 'watchdog_period' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  watchdog_.setPeriod(ros::Duration(watchdog_period));

  YAML::Node node = YAML::Load(controller_description);
  giskard::QPControllerSpec spec = node.as< giskard::QPControllerSpec >();
  controller_ = giskard::generate(spec);
  state_ = Eigen::VectorXd::Zero(joint_names_.size() + 2*6);
  controller_started_ = false;

  for (std::vector<std::string>::iterator it = joint_names_.begin(); it != joint_names_.end(); ++it)
    vel_controllers_.push_back(nh.advertise<std_msgs::Float64>("/" + it->substr(0, it->size() - 6) + "_velocity_controller/command", 1));

  feedback_pub_ = nh.advertise<giskard_msgs::ControllerFeedback>("feedback", 1);
  current_command_hash_pub_ = nh.advertise<std_msgs::UInt64>("current_command_hash", 1, true);
  current_command_pub_ = nh.advertise<giskard_msgs::WholeBodyCommand>("current_command", 1, true);
  feedback_ = initFeedbackMsg(controller_);

  ROS_DEBUG("Waiting for goal.");
  ros::Subscriber goal_sub = nh.subscribe("goal", 0, goal_callback);
  js_sub_ = nh.subscribe("joint_states", 0, js_callback);
  ros::spin();

  return 0;
}
