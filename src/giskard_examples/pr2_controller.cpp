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
#include <geometry_msgs/Point.h>
#include <yaml-cpp/yaml.h>
#include <giskard/giskard.hpp>

int nWSR_;
giskard::QPController controller_;
std::vector<std::string> joint_names_;
std::vector<ros::Publisher> vel_controllers_;
geometry_msgs::Point goal_point_;
ros::Subscriber js_sub_;
Eigen::VectorXd state_;
bool controller_started_;

void js_callback(const sensor_msgs::JointState::ConstPtr& msg)
{
  if (!controller_started_)
    return;

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

  if (controller_.update(state_, nWSR_))
  {
    Eigen::VectorXd commands = controller_.get_command();
    for (unsigned int i=0; i < vel_controllers_.size(); i++)
    {
      std_msgs::Float64 command;
      command.data = commands[i];
      vel_controllers_[i].publish(command);
    }
  }
  else
  {
    ROS_WARN("Update failed.");
    // TODO: remove or change to ros_debug
    std::cout << "State " << state_ << std::endl;
  }
}

void goal_callback(const geometry_msgs::Point::ConstPtr& msg)
{
  ROS_INFO("New goal: %f, %f, %f", msg->x, msg->y, msg->z);

  state_[joint_names_.size()] = msg->x;
  state_[joint_names_.size() + 1] = msg->y;
  state_[joint_names_.size() + 2] = msg->z;

  if (!controller_started_)
  {
    if (controller_.start(state_, nWSR_))
    {
      ROS_INFO("Controller started.");
      goal_point_ = *msg;
      controller_started_ = true;
    }
    else
    {
      ROS_ERROR("Couldn't start controller.");
    }
  }
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "pr2_controller");
  ros::NodeHandle nh("~");

  nh.param("nWSR", nWSR_, 10);

  std::string controller_description;
  if (!nh.getParam("controller_description", controller_description))
  {
    ROS_ERROR("Parameter 'controller_description' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  if (!nh.getParam("joint_names", joint_names_))
  {
    ROS_ERROR("Parameter 'joint_names' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  YAML::Node node = YAML::Load(controller_description);
  giskard::QPControllerSpec spec = node.as< giskard::QPControllerSpec >();
  controller_ = giskard::generate(spec);
  state_ = Eigen::VectorXd::Zero(joint_names_.size() + 3);
  controller_started_ = false;

  for (std::vector<std::string>::iterator it = joint_names_.begin(); it != joint_names_.end(); ++it)
    vel_controllers_.push_back(nh.advertise<std_msgs::Float64>("/" + *it + "/vel_cmd", 1));

  ROS_INFO("Waiting for goal.");
  ros::Subscriber goal_sub = nh.subscribe("goal", 0, goal_callback);
  js_sub_ = nh.subscribe("joint_states", 0, js_callback);
  ros::spin();

  return 0;
}
