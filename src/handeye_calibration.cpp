// Standard headers
#include <string>
#include <fstream>

// ROS headers
#include <ros/ros.h>
#include <ros/service.h>
#include <std_msgs/String.h>
#include <sensor_msgs/Image.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>
#include <eigen_conversions/eigen_msg.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_ros/point_cloud.h>
#include <ensenso/CalibrationMoveRandom.h>
#include <ensenso/AXXBsolver.h>
// PCL headers
#include <pcl/common/colors.h>
#include <pcl/common/transforms.h>
#include "ensenso/ensenso_grabber.h"


// Typedefs
typedef std::pair<pcl::PCLImage, pcl::PCLImage> PairOfImages;
typedef pcl::PointXYZ PointXYZ;
typedef pcl::PointCloud<PointXYZ> PointCloudXYZ;


class HandeyeCalibration
{
  private:
    // Ros
    ros::NodeHandle           nh_, nh_private_;
    ros::Publisher            l_raw_pub_;
    ros::Publisher            r_raw_pub_;
    ros::ServiceClient        move_client_, solver_client_;
    // Ensenso grabber
    pcl::EnsensoGrabber::Ptr  ensenso_ptr_;
    
  public:
     HandeyeCalibration(): 
      nh_private_("~")
    { 
      // Initialize Ensenso
      ensenso_ptr_.reset(new pcl::EnsensoGrabber);
      ensenso_ptr_->openDevice("150534");
      ensenso_ptr_->openTcpPort();
      ensenso_ptr_->configureCapture(true, true, 1, 0.32, true, 1, false, false, false, 10, false);
      // Setup image publishers
      l_raw_pub_ = nh_.advertise<sensor_msgs::Image>("left/image_raw", 2);
      r_raw_pub_ = nh_.advertise<sensor_msgs::Image>("right/image_raw", 2);

      // Start ensenso grabber
      boost::function<void
      (const boost::shared_ptr<PointCloudXYZ>&,
       const boost::shared_ptr<PairOfImages>&,const boost::shared_ptr<PairOfImages>&)> f = boost::bind (&HandeyeCalibration::grabberCallback, this, _1, _2, _3);
      ensenso_ptr_->registerCallback(f);
      ensenso_ptr_->start();
      
      // Initialize calibration
      float grid_spacing = 12.5;
      int num_pose = 150;
      Eigen::Matrix4d pattern_pose;
      pattern_pose << 0,0,1,-1,
                      0,1,0,0,
                      -1,0,0,0.8,
                      0,0,0,1;
      Eigen::Affine3d est_pattern_pose;
      est_pattern_pose= pattern_pose; //Update this later
      
      if( performCalibration(num_pose,grid_spacing,est_pattern_pose))
        ROS_INFO("DONE CALIBRATION");
      else
        ROS_ERROR("FAIL TO CALIBRATE!");
    }
    
    ~HandeyeCalibration()
    {
      ensenso_ptr_->closeTcpPort();
      ensenso_ptr_->closeDevice();
    }
    
    void grabberCallback( const boost::shared_ptr<PointCloudXYZ>& cloud,
                      const boost::shared_ptr<PairOfImages>& rawimages,  const boost::shared_ptr<PairOfImages>& rectifiedimages)
    {
      // Images
      unsigned char *l_raw_image_array = reinterpret_cast<unsigned char *>(&rawimages->first.data[0]);
      unsigned char *r_raw_image_array = reinterpret_cast<unsigned char *>(&rawimages->second.data[0]);
      int type(CV_8UC1);
      std::string encoding("mono8");
      if (rawimages->first.encoding == "CV_8UC3")
      {
        type = CV_8UC3;
        encoding = "bgr8";
      }
      cv::Mat l_raw_image(rawimages->first.height, rawimages->first.width, type, l_raw_image_array);
      cv::Mat r_raw_image(rawimages->first.height, rawimages->first.width, type, r_raw_image_array);
      std_msgs::Header header;
      header.frame_id = "world";
      header.stamp = ros::Time::now();
      l_raw_pub_.publish(cv_bridge::CvImage(header, encoding, l_raw_image).toImageMsg());
      r_raw_pub_.publish(cv_bridge::CvImage(header, encoding, r_raw_image).toImageMsg());
    }
    
    bool performCalibration(int num_pose, float grid_spacing, Eigen::Affine3d est_pattern_pose)
    {
      // Setup Ensenso
      ensenso_ptr_->stop();
      ensenso_ptr_->clearCalibrationPatternBuffer();// In case a previous calibration was launched!
        
      ensenso_ptr_->initExtrinsicCalibration(grid_spacing);
      ensenso_ptr_->start();

      // Setup services
      //random move service
      std::string randmovesrv_name = "calibration_move_random";
      move_client_ = nh_.serviceClient<ensenso::CalibrationMoveRandom>(randmovesrv_name.c_str());
      // Wait for move robot service
      ros::service::waitForService(randmovesrv_name.c_str());
      ensenso::CalibrationMoveRandom randmovesrv;
      // Convert req format to msg
      tf::poseEigenToMsg(est_pattern_pose, randmovesrv.request.patternpose);
      randmovesrv.request.minradius = 0.50;
      // Capture calibration data from the sensor, move the robot and repeat until enough data is acquired
      std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > robot_poses;
      randmovesrv.request.gotoinitpose = false;
      

      // Setup variables
      geometry_msgs::PoseArray robotposes;
      geometry_msgs::PoseArray objwrtcamposes;
      robotposes.poses.clear();
      robotposes.header.stamp = ros::Time::now();
      robotposes.header.frame_id = "/robot";
      objwrtcamposes.poses.clear();
      objwrtcamposes.header.stamp = ros::Time::now();
      objwrtcamposes.header.frame_id = "/robot";
      
      while (ros::ok() && robotposes.poses.size() < num_pose)
      { 
        ROS_INFO("Start a new collecting_data");
        ensenso_ptr_->start();
        // Move the robot to a random position (still be able to capture the pattern)
        if (move_client_.call(randmovesrv))
        {
          if (!randmovesrv.response.success)
          {
            ROS_ERROR("Random_move: failed to plan the robot!");
            return false;
          }
          sleep(0.5); 
          // Collect pattern pose
          ensenso_ptr_->stop();
          ensenso_ptr_->clearCalibrationPatternBuffer(); // Clear buffer before acquiring a new one
          int executecapturing ;
          for (int i =0; i<5; i+=1)
          {
            executecapturing = ensenso_ptr_->captureCalibrationPattern();
            if (executecapturing == -1)
            {
              ROS_WARN_STREAM("Failed to capture calibration pattern: break");
              break;
            }
            std::cout <<"No. pattern poses: "<<executecapturing<< "\n";
          }
          if (executecapturing <3)
            {ROS_WARN_STREAM("Failed to capture calibration pattern: skipping to next pose");continue;}
          Eigen::Affine3d patternwrtcam;
          geometry_msgs::Pose patternwrtcampose;
          ensenso_ptr_->estimateCalibrationPatternPose(patternwrtcam);
          tf::poseEigenToMsg(patternwrtcam, patternwrtcampose);
          objwrtcamposes.poses.push_back(patternwrtcampose);
          ROS_INFO("Pattern pose acquired!");
          ensenso_ptr_->clearCalibrationPatternBuffer();
          // Collect robot pose
          robotposes.poses.push_back(randmovesrv.response.robotpose);
          ROS_INFO("Robot pose acquired!");
          std::cout << robotposes.poses.size() << " of " << num_pose << " data acquired\n"; 
        }
        else
          ROS_ERROR("Failed to call service");
      }
      sleep(1);
      
      ROS_INFO("Calling AX=XB solver to find calibration matrix...");
      //Solver service
      std::string solversrv_name = "AXXBsolver";
      solver_client_ = nh_.serviceClient<ensenso::AXXBsolver>(solversrv_name.c_str());
      // Wait for move robot service
      ros::service::waitForService(solversrv_name.c_str());
      ensenso::AXXBsolver solversrv;
      solversrv.request.robotposes = robotposes;
      solversrv.request.objwrtcamposes = objwrtcamposes;
      if (solver_client_.call(solversrv))
        {
        ROS_INFO("Calibration computation successful!");
        Eigen::Affine3d result;
        tf::poseMsgToEigen(solversrv.response.X, result);
        std::cout << "Translation:\n" << result.translation() <<"\n";
        std::cout << "Rotation:\n" << result.rotation() <<"\n";
        // Save the calibration result as another format?
        ///////////////////////////////////////////////
        return true;
        }
      else
        ROS_ERROR("Failed to call service");
      return false;
      
    }

};

int main(int argc, char **argv)
{
  ros::init (argc, argv, "handeye_calibration_node");
  HandeyeCalibration cal;
  ros::spin();
  ros::shutdown();
  return 0;
}