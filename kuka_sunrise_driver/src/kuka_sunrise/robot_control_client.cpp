// Copyright 2020 Zoltán Rési
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "kuka_sunrise/robot_control_client.hpp"

namespace kuka_sunrise
{
CallbackReturn RobotControlClient::on_init(const hardware_interface::HardwareInfo & system_info)
{
// TODO(Svastits): add parameter for command mode, receive multiplier etc

  if (hardware_interface::SystemInterface::on_init(system_info) != CallbackReturn::SUCCESS) {
    return CallbackReturn::ERROR;
  }
  hw_states_.resize(info_.joints.size());
  hw_commands_.resize(info_.joints.size());
  hw_torques_.resize(info_.joints.size());
  hw_effort_command_.resize(info_.joints.size());

  for (const hardware_interface::ComponentInfo & joint : info_.joints) {
    if (joint.command_interfaces.size() != 2) {
      RCLCPP_FATAL(
        rclcpp::get_logger("RobotControlClient"),
        "expecting exactly 2 command interface");
      return CallbackReturn::ERROR;
    }

    if (joint.command_interfaces[0].name != hardware_interface::HW_IF_POSITION) {
      RCLCPP_FATAL(
        rclcpp::get_logger(
          "RobotControlClient"), "expecting POSITION command interface as first");
      return CallbackReturn::ERROR;
    }

    if (joint.command_interfaces[1].name != hardware_interface::HW_IF_EFFORT) {
      RCLCPP_FATAL(
        rclcpp::get_logger(
          "RobotControlClient"), "expecting EFFORT command interface as second");
      return CallbackReturn::ERROR;
    }

    if (joint.state_interfaces.size() != 2) {
      RCLCPP_FATAL(rclcpp::get_logger("RobotControlClient"), "expecting exactly 2 state interface");
      return CallbackReturn::ERROR;
    }

    if (joint.state_interfaces[0].name != hardware_interface::HW_IF_POSITION) {
      RCLCPP_FATAL(
        rclcpp::get_logger(
          "RobotControlClient"), "expecting POSITION state interface as first");
      return CallbackReturn::ERROR;
    }

    if (joint.state_interfaces[1].name != hardware_interface::HW_IF_EFFORT) {
      RCLCPP_FATAL(
        rclcpp::get_logger(
          "RobotControlClient"), "expecting EFFORT state interface as second");
      return CallbackReturn::ERROR;
    }
    // TODO(Svastits): add external torque interface to URDF and check it here
  }

  return CallbackReturn::SUCCESS;
}

CallbackReturn RobotControlClient::on_configure(const rclcpp_lifecycle::State &)
{
  return CallbackReturn::SUCCESS;
}

CallbackReturn RobotControlClient::on_activate(const rclcpp_lifecycle::State &)
{
  RCLCPP_INFO(rclcpp::get_logger("HWIF"),"activating client");
  if(!client_application_.connect(30200, nullptr))
  {
	  RCLCPP_ERROR(rclcpp::get_logger("HWIF"),"could not connect");
	  return CallbackReturn::FAILURE;
  }
  activate();
  RCLCPP_INFO(rclcpp::get_logger("HWIF"),"activated client");
  return CallbackReturn::SUCCESS;
}

CallbackReturn RobotControlClient::on_deactivate(const rclcpp_lifecycle::State &)
{
  client_application_.disconnect();
  deactivate();
  return CallbackReturn::SUCCESS;
}

RobotControlClient::~RobotControlClient()
{
}

bool RobotControlClient::activate()
{
  this->ActivatableInterface::activate();
  return true;  // TODO(resizoltan) check if successful
}

bool RobotControlClient::deactivate()
{
  this->ActivatableInterface::deactivate();
  return true;  // TODO(resizoltan) check if successful
}

void RobotControlClient::waitForCommand()
{
  // TODO(Svastits): is this really the purpose of waitForCommand?
  rclcpp::Time stamp = ros_clock_.now();
  if (++receive_counter_ == receive_multiplier_) {
    updateCommand(stamp);
    receive_counter_ = 0;
  }
}

void RobotControlClient::command()
{
  rclcpp::Time stamp = ros_clock_.now();
  if (++receive_counter_ == receive_multiplier_) {
    updateCommand(stamp);
    receive_counter_ = 0;
  }
}


hardware_interface::return_type RobotControlClient::read(
  const rclcpp::Time &,
  const rclcpp::Duration &)
{
  if (!is_active_) {
	RCLCPP_ERROR(rclcpp::get_logger("ClientApplication"), "Controller not active");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return hardware_interface::return_type::ERROR;
  }

  if (!client_application_.client_app_read()) {
    RCLCPP_ERROR(rclcpp::get_logger("ClientApplication"), "Failed to read data from controller");
    return hardware_interface::return_type::ERROR;
  }

  // get the position and efforts and share them with exposed state interfaces
  const double * position = robotState().getMeasuredJointPosition();
  hw_states_.assign(position, position + KUKA::FRI::LBRState::NUMBER_OF_JOINTS);
  const double * torque = robotState().getMeasuredTorque();
  hw_torques_.assign(torque, torque + KUKA::FRI::LBRState::NUMBER_OF_JOINTS);

  tracking_performance_ = robotState().getTrackingPerformance();
  fri_state_ = robotState().getSessionState();
  // RCLCPP_INFO(rclcpp::get_logger("ClientApplication"), "FRI state: %i", fri_state_);

  // const double* external_torque = robotState().getExternalTorque();
  // hw_torques_ext_.assign(external_torque, external_torque+KUKA::FRI::LBRState::NUMBER_OF_JOINTS);
  // TODO(Svastits): add external torque interface
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type RobotControlClient::write(
  const rclcpp::Time &,
  const rclcpp::Duration &)
{
  if (!is_active_) {
    RCLCPP_INFO(rclcpp::get_logger("RobotControlClient"), "Controller deactivated");
    return hardware_interface::return_type::ERROR;
  }

  // Call the appropriate callback for the actual state (e.g. updateCommand)
  // this updates the command to be sent based on the output of the controller update
  client_application_.client_app_update();

  client_application_.client_app_write();

  return hardware_interface::return_type::OK;
}

void RobotControlClient::updateCommand(const rclcpp::Time &)
{
  if (!is_active_) {
    printf("client deactivated, exiting updateCommand\n");
    return;
  }

  if (torque_command_mode_) {
    const double * joint_torques_ = hw_effort_command_.data();
    robotCommand().setJointPosition(robotState().getIpoJointPosition());
    robotCommand().setTorque(joint_torques_);
  } else {
    const double * joint_positions_ = hw_commands_.data();
    robotCommand().setJointPosition(joint_positions_);
  }
  // TODO(Svastits): setDigitalIOValue and setAnalogIOValue
/*
  for (auto & output_subscription : output_subscriptions_) {
    output_subscription->updateOutput();
  }
  */
}

std::vector<hardware_interface::StateInterface> RobotControlClient::export_state_interfaces()
{

  // TODO(Svastits): add FRI state interface
  RCLCPP_INFO(rclcpp::get_logger("RobotControlClient"), "export_state_interfaces()");

  std::vector<hardware_interface::StateInterface> state_interfaces;

  state_interfaces.emplace_back(
        hardware_interface::StateInterface("state", "fri_state", &fri_state_));
  for (size_t i = 0; i < info_.joints.size(); i++) {
    state_interfaces.emplace_back(
      hardware_interface::StateInterface(
        info_.joints[i].name,
        hardware_interface::HW_IF_POSITION,
        &hw_states_[i]));

    state_interfaces.emplace_back(
      hardware_interface::StateInterface(
        info_.joints[i].name,
        hardware_interface::HW_IF_EFFORT,
        &hw_torques_[i]));
  }
  return state_interfaces;
}

std::vector<hardware_interface::CommandInterface> RobotControlClient::export_command_interfaces()
{
  RCLCPP_INFO(rclcpp::get_logger("RobotControlClient"), "export_command_interfaces()");

  std::vector<hardware_interface::CommandInterface> command_interfaces;

  command_interfaces.emplace_back(
        hardware_interface::CommandInterface("timing","receive_multiplier", &receive_multiplier_));
  for (size_t i = 0; i < info_.joints.size(); i++) {
    command_interfaces.emplace_back(
      hardware_interface::CommandInterface(
        info_.joints[i].name,
        hardware_interface::HW_IF_POSITION,
        &hw_commands_[i]));

    command_interfaces.emplace_back(
      hardware_interface::CommandInterface(
        info_.joints[i].name,
        hardware_interface::HW_IF_EFFORT,
        &hw_effort_command_[i]));
  }
  return command_interfaces;
}
}  // namespace kuka_sunrise

#include "pluginlib/class_list_macros.hpp"


PLUGINLIB_EXPORT_CLASS(
  kuka_sunrise::RobotControlClient,
  hardware_interface::SystemInterface
)
