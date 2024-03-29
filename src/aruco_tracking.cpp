/*********************************************************************************************//**
* @file aruco_mapping.cpp
*
* Copyright (c)
* Smart Robotic Systems
* March 2015
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* Redistributions of source code must retain the above copyright notice, this
* list of conditions and the following disclaimer.
*
* Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

/* Author: Jan Bacik */

#ifndef ARUCO_TRACKING_CPP
#define ARUCO_TRACKING_CPP

#include <aruco_tracking.h>

namespace aruco_tracking
{

ArucoTracking::ArucoTracking(ros::NodeHandle *nh) :
  listener_ (new tf::TransformListener),  // Initialize TF Listener
  num_of_markers_ (10),                   // Number of used markers
  marker_size_(0.1),                      // Marker size in m
  calib_filename_("empty"),               // Calibration filepath
  space_type_ ("plane"),                  // Space type - 2D plane
  roi_allowed_ (false),                   // ROI not allowed by default
  first_marker_detected_(false),          // First marker not detected by defualt
  lowest_marker_id_(-1),                  // Lowest marker ID
  closest_camera_index_(0)                // Reset closest camera index

{
  double temp_marker_size;

  //Parse params from launch file
  nh->getParam("/aruco_tracking/calibration_file", calib_filename_);
  nh->getParam("/aruco_tracking/marker_size", temp_marker_size);
  nh->getParam("/aruco_tracking/num_of_markers", num_of_markers_);
  nh->getParam("/aruco_tracking/space_type",space_type_);
  nh->getParam("/aruco_tracking/roi_allowed",roi_allowed_);
  nh->getParam("/aruco_tracking/roi_x",roi_x_);
  nh->getParam("/aruco_tracking/roi_y",roi_y_);
  nh->getParam("/aruco_tracking/roi_w",roi_w_);
  nh->getParam("/aruco_tracking/roi_h",roi_h_);

  // Double to float conversion
  marker_size_ = float(temp_marker_size);

  if(calib_filename_ == "empty")
    ROS_WARN("Calibration filename empty! Check the launch file paths");
  else
  {
    ROS_INFO_STREAM("Calibration file path: " << calib_filename_ );
    ROS_INFO_STREAM("Number of markers: " << num_of_markers_);
    ROS_INFO_STREAM("Marker Size: " << marker_size_);
    ROS_INFO_STREAM("Type of space: " << space_type_);
    ROS_INFO_STREAM("ROI allowed: " << roi_allowed_);
    ROS_INFO_STREAM("ROI x-coor: " << roi_x_);
    ROS_INFO_STREAM("ROI y-coor: " << roi_x_);
    ROS_INFO_STREAM("ROI width: "  << roi_w_);
    ROS_INFO_STREAM("ROI height: " << roi_h_);
  }

  //ROS publishers
  marker_msg_pub_           = nh->advertise<aruco_tracking::ArucoMarker>("aruco_poses",1);
  marker_visualization_pub_ = nh->advertise<visualization_msgs::Marker>("aruco_markers",1);

  //Parse data from calibration file
  parseCalibrationFile(calib_filename_);

  //Initialize OpenCV window
  cv::namedWindow("Mono8", CV_WINDOW_AUTOSIZE);
}

ArucoTracking::~ArucoTracking()
{
 delete listener_;
}

bool
ArucoTracking::parseCalibrationFile(std::string calib_filename)
{
  sensor_msgs::CameraInfo camera_calibration_data;
  std::string camera_name = "camera";

  camera_calibration_parsers::readCalibrationIni(calib_filename, camera_name, camera_calibration_data);

  // Alocation of memory for calibration data
  cv::Mat  *intrinsics       = new(cv::Mat)(3, 3, CV_64F);
  cv::Mat  *distortion_coeff = new(cv::Mat)(5, 1, CV_64F);
  cv::Size *image_size       = new(cv::Size);

  image_size->width = camera_calibration_data.width;
  image_size->height = camera_calibration_data.height;

  for(size_t i = 0; i < 3; i++)
    for(size_t j = 0; j < 3; j++)
    intrinsics->at<double>(i,j) = camera_calibration_data.K.at(3*i+j);

  for(size_t i = 0; i < 5; i++)
    distortion_coeff->at<double>(i,0) = camera_calibration_data.D.at(i);

  ROS_DEBUG_STREAM("Image width: " << image_size->width);
  ROS_DEBUG_STREAM("Image height: " << image_size->height);
  ROS_DEBUG_STREAM("Intrinsics:" << std::endl << *intrinsics);
  ROS_DEBUG_STREAM("Distortion: " << *distortion_coeff);


  //Load parameters to aruco_calib_param_ for aruco detection
  aruco_calib_params_.setParams(*intrinsics, *distortion_coeff, *image_size);

  //Simple check if calibration data meets expected values
  if ((intrinsics->at<double>(2,2) == 1) && (distortion_coeff->at<double>(0,4) == 0))
  {
    ROS_INFO_STREAM("Calibration data loaded successfully");
    return true;
  }
  else
  {
    ROS_WARN("Wrong calibration data, check calibration file and filepath");
    return false;
  }
}

void
ArucoTracking::imageCallback(const sensor_msgs::ImageConstPtr &original_image)
{
  //Create cv_brigde instance
  cv_bridge::CvImagePtr cv_ptr;
  try
  {
    cv_ptr=cv_bridge::toCvCopy(original_image, sensor_msgs::image_encodings::MONO8);
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("Not able to convert sensor_msgs::Image to OpenCV::Mat format %s", e.what());
    return;
  }

  // sensor_msgs::Image to OpenCV Mat structure
  cv::Mat I = cv_ptr->image;

  // region of interest
  if(roi_allowed_==true)
    I = cv_ptr->image(cv::Rect(roi_x_,roi_y_,roi_w_,roi_h_));

  //Marker detection
  processImage(I,I);

  // Show image
  cv::imshow("Mono8", I);
  cv::waitKey(10);
}


bool
ArucoTracking::processImage(cv::Mat input_image,cv::Mat output_image)
{
  aruco::MarkerDetector Detector;
  std::vector<aruco::Marker> real_time_markers;

  //Set visibility flag to false for all markers
  for (std::map<int, MarkerInfo>::iterator it=markers_.begin(); it!=markers_.end(); ++it)
  {
    it->second.visible = false;
  }

  // Detect markers
  Detector.detect(input_image,real_time_markers,aruco_calib_params_,marker_size_);

  // If no marker found, print statement
  if(real_time_markers.size() == 0)
    ROS_DEBUG("No marker found!");

  //------------------------------------------------------
  // FIRST MARKER DETECTED
  //------------------------------------------------------
  if((real_time_markers.size() > 0) && (first_marker_detected_ == false))
  {
    first_marker_detected_=true;
    detectFirstMarker(real_time_markers);
  }
  //------------------------------------------------------
  // FOR EVERY MARKER DO
  //------------------------------------------------------
  for(size_t i = 0; i < real_time_markers.size();i++)
  {
    int current_marker_id = real_time_markers[i].id;

    //Draw marker convex, ID, cube and axis
    real_time_markers[i].draw(output_image, cv::Scalar(0,0,255),2);
    aruco::CvDrawingUtils::draw3dCube(output_image,real_time_markers[i], aruco_calib_params_);
    aruco::CvDrawingUtils::draw3dAxis(output_image,real_time_markers[i], aruco_calib_params_);

    // // Existing marker ?
    if(isDetected(current_marker_id))
    {
      ROS_DEBUG_STREAM("Existing marker with ID: " << current_marker_id << " found");
      setCurrentCameraPose(real_time_markers[i], current_marker_id, true);
    }
    else
    {
      /// new marker
      MarkerInfo marker;
      marker.marker_id = current_marker_id;
      markers_[current_marker_id] = marker;
      // existing = true;
      ROS_DEBUG_STREAM("New marker with ID: " << current_marker_id << " found");
    }

    // Change visibility flag of new marker
    markVisible(real_time_markers);
    //------------ ------------------------------------------
    if((markers_[current_marker_id].previous_marker_id == -1) && (first_marker_detected_ == true) && (current_marker_id != lowest_marker_id_))
    {
      //markers_[current_marker_id].current_camera_tf=arucoMarker2Tf(real_time_markers[i]);
      setCurrentCameraPose(real_time_markers[i], current_marker_id, false);

      // Flag to keep info if any_known marker_visible in actual image
      bool any_known_marker_visible = false;

      // Array ID of markers, which position of new marker is calculated
      int last_marker_id;

      // Testing, if is possible calculate position of a new marker to old known marker
      knownMarkerInImage(any_known_marker_visible, last_marker_id, current_marker_id);

     // New position can be calculated
     if(any_known_marker_visible == true)
     {
       // Generating TFs for listener
       publishCameraMarkerTransforms(current_marker_id, last_marker_id);
        // Save origin and quaternion of calculated TF
        tf::Vector3 marker_origin = markers_[current_marker_id].tf_to_previous.getOrigin();
        tf::Quaternion marker_quaternion = markers_[current_marker_id].tf_to_previous.getRotation();
        // If plane type selected roll, pitch and Z axis are zero
        if(space_type_ == "plane")
        {
          double roll, pitch, yaw;
          tf::Matrix3x3(marker_quaternion).getRPY(roll,pitch,yaw);
          roll = 0;
          pitch = 0;
          marker_origin.setZ(0);
          marker_quaternion.setRPY(pitch,roll,yaw);
        }

        markers_[current_marker_id].tf_to_previous.setRotation(marker_quaternion);
        markers_[current_marker_id].tf_to_previous.setOrigin(marker_origin);

        marker_origin = markers_[current_marker_id].tf_to_previous.getOrigin();
        markers_[current_marker_id].geometry_msg_to_previous.position.x = marker_origin.getX();
        markers_[current_marker_id].geometry_msg_to_previous.position.y = marker_origin.getY();
        markers_[current_marker_id].geometry_msg_to_previous.position.z = marker_origin.getZ();
        //
        marker_quaternion = markers_[current_marker_id].tf_to_previous.getRotation();
        markers_[current_marker_id].geometry_msg_to_previous.orientation.x = marker_quaternion.getX();
        markers_[current_marker_id].geometry_msg_to_previous.orientation.y = marker_quaternion.getY();
        markers_[current_marker_id].geometry_msg_to_previous.orientation.z = marker_quaternion.getZ();
        markers_[current_marker_id].geometry_msg_to_previous.orientation.w = marker_quaternion.getW();
        //
        setCameraPose(current_marker_id, true);
        //
        // Publish all TFs and markers
        publishTfs(false);
      }
    }

    //------------------------------------------------------
    // Compute global position of new marker
    //-----------------------------------------------------
    computeGlobalMarkerPose(current_marker_id);
  }


  //After For Loop Code
  //------------------------------------------------------
  // Compute which of visible markers is the closest to the camera
  //------------------------------------------------------
  bool any_markers_visible=false;
  int num_of_visible_markers=0;
  nearestMarkersToCamera(any_markers_visible, num_of_visible_markers);

  //------------------------------------------------------
  // Compute global camera pose
  //------------------------------------------------------
  computeGlobalCameraPose(any_markers_visible);

  //------------------------------------------------------
  // Publish all known markers
  //------------------------------------------------------
  if(first_marker_detected_ == true)
    publishTfs(true);

  //------------------------------------------------------
  // Publish custom marker message
  //------------------------------------------------------
  publishCustomMarker(any_markers_visible, num_of_visible_markers);

  //--------------------------------------
  // Reset Markers
  //--------------------------------------
  for (auto itr = markers_.begin(); itr != markers_.end(); ++itr) {
    if(itr->first != lowest_marker_id_){
      markers_.erase(itr->first);
    }
  }
  return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////
void
ArucoTracking::knownMarkerInImage(bool &any_known_marker_visible, int &last_marker_id, int current_marker_id)
{
  if (markers_[lowest_marker_id_].visible==true)
  {
    any_known_marker_visible = true;
    markers_[current_marker_id].previous_marker_id = lowest_marker_id_;//it->first;
    last_marker_id = lowest_marker_id_;
  }
}
//////////////////////////////////////////////////////////////////////////
void
ArucoTracking::publishCameraMarkerTransforms(int current_marker_id, int last_marker_id)
{
  // Naming - TFs
  std::stringstream camera_tf_id;
  std::stringstream camera_tf_id_old;
  std::stringstream marker_tf_id_old;

  camera_tf_id << "camera_" << current_marker_id;
  camera_tf_id_old << "camera_" << last_marker_id;
  marker_tf_id_old << "marker_" << last_marker_id;
  for(char k = 0; k < 10; k++)
  {
    // TF from old marker and its camera
    broadcaster_.sendTransform(tf::StampedTransform(markers_[last_marker_id].current_camera_tf,ros::Time::now(),
                                                    marker_tf_id_old.str(),camera_tf_id_old.str()));

    // TF from old camera to new camera
    broadcaster_.sendTransform(tf::StampedTransform(markers_[current_marker_id].current_camera_tf,ros::Time::now(),
                                                    camera_tf_id_old.str(),camera_tf_id.str()));

    ros::Duration(BROADCAST_WAIT_INTERVAL).sleep();
  }

   // Calculate TF between two markers
   listener_->waitForTransform(marker_tf_id_old.str(),camera_tf_id.str(),ros::Time(0),
                               ros::Duration(WAIT_FOR_TRANSFORM_INTERVAL));
   try
   {
     broadcaster_.sendTransform(tf::StampedTransform(markers_[last_marker_id].current_camera_tf,ros::Time::now(),
                                                     marker_tf_id_old.str(),camera_tf_id_old.str()));

     broadcaster_.sendTransform(tf::StampedTransform(markers_[current_marker_id].current_camera_tf,ros::Time::now(),
                                                     camera_tf_id_old.str(),camera_tf_id.str()));

     listener_->lookupTransform(marker_tf_id_old.str(),camera_tf_id.str(),ros::Time(0),
                                markers_[current_marker_id].tf_to_previous);
   }
   catch(tf::TransformException &e)
   {
     ROS_ERROR("Not able to lookup transform");
   }
}


void
ArucoTracking::computeGlobalMarkerPose(int current_marker_id)
{
  if(first_marker_detected_ == true)
  {
    // Publish all TF five times for listener
    for(char k = 0; k < 5; k++)
      publishTfs(false);

    std::stringstream marker_tf_name;
    marker_tf_name << "marker_" << current_marker_id;
    listener_->waitForTransform("world",marker_tf_name.str(),ros::Time(0),
                                ros::Duration(WAIT_FOR_TRANSFORM_INTERVAL));
    try
    {
      listener_->lookupTransform("world",marker_tf_name.str(),ros::Time(0),
                                 markers_[current_marker_id].tf_to_world);
    }
    catch(tf::TransformException &e)
    {
      ROS_ERROR("Not able to lookup transform");
    }

    // Saving TF to Pose
    const tf::Vector3 marker_origin = markers_[current_marker_id].tf_to_world.getOrigin();
    markers_[current_marker_id].geometry_msg_to_world.position.x = marker_origin.getX();
    markers_[current_marker_id].geometry_msg_to_world.position.y = marker_origin.getY();
    markers_[current_marker_id].geometry_msg_to_world.position.z = marker_origin.getZ();

    tf::Quaternion marker_quaternion=markers_[current_marker_id].tf_to_world.getRotation();
    markers_[current_marker_id].geometry_msg_to_world.orientation.x = marker_quaternion.getX();
    markers_[current_marker_id].geometry_msg_to_world.orientation.y = marker_quaternion.getY();
    markers_[current_marker_id].geometry_msg_to_world.orientation.z = marker_quaternion.getZ();
    markers_[current_marker_id].geometry_msg_to_world.orientation.w = marker_quaternion.getW();
  }
}
///////////////////////////////////////////////////////////////////////////////
void
ArucoTracking::computeGlobalCameraPose(bool any_markers_visible)
{
  if((first_marker_detected_ == true) && (any_markers_visible == true))
  {
    std::stringstream closest_camera_tf_name;
    closest_camera_tf_name << "camera_" << closest_camera_index_;

    listener_->waitForTransform("world",closest_camera_tf_name.str(),ros::Time(0),
                                ros::Duration(WAIT_FOR_TRANSFORM_INTERVAL));
    try
    {
      listener_->lookupTransform("world",closest_camera_tf_name.str(),ros::Time(0),
                                 world_position_transform_);
    }
    catch(tf::TransformException &ex)
    {
      ROS_ERROR("Not able to lookup transform");
    }

    // Saving TF to Pose
    const tf::Vector3 marker_origin = world_position_transform_.getOrigin();
    world_position_geometry_msg_.position.x = marker_origin.getX();
    world_position_geometry_msg_.position.y = marker_origin.getY();
    world_position_geometry_msg_.position.z = marker_origin.getZ();

    tf::Quaternion marker_quaternion = world_position_transform_.getRotation();
    world_position_geometry_msg_.orientation.x = marker_quaternion.getX();
    world_position_geometry_msg_.orientation.y = marker_quaternion.getY();
    world_position_geometry_msg_.orientation.z = marker_quaternion.getZ();
    world_position_geometry_msg_.orientation.w = marker_quaternion.getW();
  }
}
/////////////////////////////////////////////////////////////


void
ArucoTracking::nearestMarkersToCamera(bool &any_markers_visible, int &num_of_visible_markers)
{
  if(first_marker_detected_ == true)
  {
    double minimal_distance = INIT_MIN_SIZE_VALUE;
    for (std::map<int, MarkerInfo>::iterator it=markers_.begin(); it!=markers_.end(); ++it)
    {
      int k = it->first;
      double a,b,c,size;;
      // If marker is visible, distance is calculated
      if(markers_[k].visible==true)
      {
        a = markers_[k].current_camera_pose.position.x;
        b = markers_[k].current_camera_pose.position.y;
        c = markers_[k].current_camera_pose.position.z;
        size = std::sqrt((a * a) + (b * b) + (c * c));
        if(size < minimal_distance)
        {
          minimal_distance = size;
          closest_camera_index_ = k;
        }

        any_markers_visible = true;
        num_of_visible_markers++;
      }
    }
  }
}

void
ArucoTracking::publishCustomMarker(bool any_markers_visible, int num_of_visible_markers)
{
  aruco_tracking::ArucoMarker marker_msg;
  if((any_markers_visible == true))
  {
    marker_msg.header.stamp = ros::Time::now();
    marker_msg.header.frame_id = "world";
    marker_msg.marker_visibile = true;
    marker_msg.num_of_visible_markers = num_of_visible_markers;
    marker_msg.global_camera_pose = world_position_geometry_msg_;
    marker_msg.marker_ids.clear();
    marker_msg.global_marker_poses.clear();
    for (std::map<int, MarkerInfo>::iterator it=markers_.begin(); it!=markers_.end(); ++it)
    {
      int j =it->first;
      if(markers_[j].visible == true)
      {
        marker_msg.marker_ids.push_back(markers_[j].marker_id);
        marker_msg.global_marker_poses.push_back(markers_[j].geometry_msg_to_world);
      }
    }
  }
  else
  {
    marker_msg.header.stamp = ros::Time::now();
    marker_msg.header.frame_id = "world";
    marker_msg.num_of_visible_markers = num_of_visible_markers;
    marker_msg.marker_visibile = false;
    marker_msg.marker_ids.clear();
    marker_msg.global_marker_poses.clear();
  }

  // Publish custom marker msg
  marker_msg_pub_.publish(marker_msg);
}

void
ArucoTracking::detectFirstMarker(std::vector<aruco::Marker> &real_time_markers)
{
  lowest_marker_id_ = real_time_markers[0].id;
  for(size_t i = 0; i < real_time_markers.size();i++)
  {
    if(real_time_markers[i].id < lowest_marker_id_)
      lowest_marker_id_ = real_time_markers[i].id;
  }
  ROS_DEBUG_STREAM("The lowest Id marker " << lowest_marker_id_ );
  MarkerInfo marker;
   // Identify lowest marker ID with world's origin
  marker.marker_id = lowest_marker_id_;

  marker.geometry_msg_to_world.position.x = 0;
  marker.geometry_msg_to_world.position.y = 0;
  marker.geometry_msg_to_world.position.z = 0;

  marker.geometry_msg_to_world.orientation.x = 0;
  marker.geometry_msg_to_world.orientation.y = 0;
  marker.geometry_msg_to_world.orientation.z = 0;
  marker.geometry_msg_to_world.orientation.w = 1;

   // Relative position and Global position
  marker.geometry_msg_to_previous.position.x = 0;
  marker.geometry_msg_to_previous.position.y = 0;
  marker.geometry_msg_to_previous.position.z = 0;

  marker.geometry_msg_to_previous.orientation.x = 0;
  marker.geometry_msg_to_previous.orientation.y = 0;
  marker.geometry_msg_to_previous.orientation.z = 0;
  marker.geometry_msg_to_previous.orientation.w = 1;

   // Transformation Pose to TF
  tf::Vector3 position;
  position.setX(0);
  position.setY(0);
  position.setZ(0);

  tf::Quaternion rotation;
  rotation.setX(0);
  rotation.setY(0);
  rotation.setZ(0);
  rotation.setW(1);

  marker.tf_to_previous.setOrigin(position);
  marker.tf_to_previous.setRotation(rotation);

   // Relative position of first marker equals Global position
  marker.tf_to_world=marker.tf_to_previous;


   // Set sign of visibility of first marker
  marker.visible=true;
   //First marker does not have any previous marker
  marker.previous_marker_id = THIS_IS_FIRST_MARKER;
  markers_[lowest_marker_id_] = marker;
  ROS_INFO_STREAM("First marker with ID: " << markers_[lowest_marker_id_].marker_id << " detected");
}
/////////////////////////////////////////////////////////////////////////////


void
ArucoTracking::setCurrentCameraPose(aruco::Marker &real_time_marker, int current_marker_id, bool inverse)
{
  if (first_marker_detected_ == true && real_time_marker.id == current_marker_id)
  {
    markers_[current_marker_id].current_camera_tf = arucoMarker2Tf(real_time_marker);
    setCameraPose(current_marker_id, inverse);
  }
}
/////////////////////////////////////////////
void
ArucoTracking::setCameraPose(int current_marker_id, bool inverse)
{
  // Invert and position of marker to compute camera pose above it
  if(inverse)
  {
    markers_[current_marker_id].current_camera_tf = markers_[current_marker_id].current_camera_tf.inverse();
  }
  const tf::Vector3 marker_origin = markers_[current_marker_id].current_camera_tf.getOrigin();
  markers_[current_marker_id].current_camera_pose.position.x = marker_origin.getX();
  markers_[current_marker_id].current_camera_pose.position.y = marker_origin.getY();
  markers_[current_marker_id].current_camera_pose.position.z = marker_origin.getZ();

  const tf::Quaternion marker_quaternion = markers_[current_marker_id].current_camera_tf.getRotation();
  markers_[current_marker_id].current_camera_pose.orientation.x = marker_quaternion.getX();
  markers_[current_marker_id].current_camera_pose.orientation.y = marker_quaternion.getY();
  markers_[current_marker_id].current_camera_pose.orientation.z = marker_quaternion.getZ();
  markers_[current_marker_id].current_camera_pose.orientation.w = marker_quaternion.getW();
}

void
ArucoTracking::markVisible(std::vector<aruco::Marker> &real_time_markers)
{
  //This function marks the previously detected markers visible i.e, whether already detected markers
  // are in the current image or not.
  for(size_t k = 0;k < real_time_markers.size(); k++)
  {
    if (markers_.count(real_time_markers[k].id)> 0)
    {
       markers_[real_time_markers[k].id].visible = true;
    }
  }
}
//////////////////////////////////////////////////////////////

bool
ArucoTracking::isDetected(int marker_id)
{
  return markers_.count(marker_id) > 0;
}
//////////////////////////////////////////////

void
ArucoTracking::publishTfs(bool world_option)
{
  for(std::map<int, MarkerInfo>::iterator it=markers_.begin(); it!=markers_.end(); ++it)
  {
    int i = it->first;
    // Actual Marker
    std::stringstream marker_tf_id;
    marker_tf_id << "marker_" << i;

    // Older marker - or World
    std::stringstream marker_tf_id_old;
    if(i == lowest_marker_id_)
      marker_tf_id_old << "world";
    else
      marker_tf_id_old << "marker_" << markers_[i].previous_marker_id;
    broadcaster_.sendTransform(tf::StampedTransform(markers_[i].tf_to_previous, ros::Time::now() ,marker_tf_id_old.str(), marker_tf_id.str()));

    // Position of camera to its marker
    std::stringstream camera_tf_id;
    camera_tf_id << "camera_" << i;
    broadcaster_.sendTransform(tf::StampedTransform(markers_[i].current_camera_tf,ros::Time::now(),marker_tf_id.str(),camera_tf_id.str()));

    if(world_option == true)
    {
      // Global position of marker TF
      std::stringstream marker_globe;
      marker_globe << "marker_globe_" << i;
      broadcaster_.sendTransform(tf::StampedTransform(markers_[i].tf_to_world,ros::Time::now(),"world",marker_globe.str()));
    }

    // Cubes for RVIZ - markers
    publishMarker(markers_[i].geometry_msg_to_previous, markers_[i].marker_id);
  }

  // Global Position of object
  if(world_option == true)
    broadcaster_.sendTransform(tf::StampedTransform(world_position_transform_,ros::Time::now(),"world","camera_position"));
}

////////////////////////////////////////////////////////////////////////////////////////////////

void
ArucoTracking::publishMarker(geometry_msgs::Pose marker_pose, int marker_id)
{
  visualization_msgs::Marker vis_marker;

  if(marker_id == lowest_marker_id_)
    vis_marker.header.frame_id = "world";
  else
  {
    std::stringstream marker_tf_id_old;
    marker_tf_id_old << "marker_" << markers_[marker_id].previous_marker_id;
    vis_marker.header.frame_id = marker_tf_id_old.str();
  }

  vis_marker.header.stamp = ros::Time::now();
  vis_marker.ns = "basic_shapes";
  vis_marker.id = marker_id;
  vis_marker.type = visualization_msgs::Marker::CUBE;
  vis_marker.action = visualization_msgs::Marker::ADD;

  vis_marker.pose = marker_pose;
  vis_marker.scale.x = marker_size_;
  vis_marker.scale.y = marker_size_;
  vis_marker.scale.z = RVIZ_MARKER_HEIGHT;

  vis_marker.color.r = RVIZ_MARKER_COLOR_R;
  vis_marker.color.g = RVIZ_MARKER_COLOR_G;
  vis_marker.color.b = RVIZ_MARKER_COLOR_B;
  vis_marker.color.a = RVIZ_MARKER_COLOR_A;

  vis_marker.lifetime = ros::Duration(RVIZ_MARKER_LIFETIME);

  marker_visualization_pub_.publish(vis_marker);
}

////////////////////////////////////////////////////////////////////////////////////////////////

tf::Transform
ArucoTracking::arucoMarker2Tf(const aruco::Marker &marker)
{
  cv::Mat marker_rotation(3,3, CV_32FC1);
  cv::Rodrigues(marker.Rvec, marker_rotation);
  cv::Mat marker_translation = marker.Tvec;

  cv::Mat rotate_to_ros(3,3,CV_32FC1);
  rotate_to_ros.at<float>(0,0) = -1.0;
  rotate_to_ros.at<float>(0,1) = 0;
  rotate_to_ros.at<float>(0,2) = 0;
  rotate_to_ros.at<float>(1,0) = 0;
  rotate_to_ros.at<float>(1,1) = 0;
  rotate_to_ros.at<float>(1,2) = 1.0;
  rotate_to_ros.at<float>(2,0) = 0.0;
  rotate_to_ros.at<float>(2,1) = 1.0;
  rotate_to_ros.at<float>(2,2) = 0.0;

  marker_rotation = marker_rotation * rotate_to_ros;//.t();

  // Origin solution
  tf::Matrix3x3 marker_tf_rot(marker_rotation.at<float>(0,0),marker_rotation.at<float>(0,1),marker_rotation.at<float>(0,2),
                              marker_rotation.at<float>(1,0),marker_rotation.at<float>(1,1),marker_rotation.at<float>(1,2),
                              marker_rotation.at<float>(2,0),marker_rotation.at<float>(2,1),marker_rotation.at<float>(2,2));

  tf::Vector3 marker_tf_tran(marker_translation.at<float>(0,0),
                             marker_translation.at<float>(1,0),
                             marker_translation.at<float>(2,0));

  return tf::Transform(marker_tf_rot, marker_tf_tran);
}



}  //aruco_mapping

#endif  //ARUCO_MAPPING_CPP
