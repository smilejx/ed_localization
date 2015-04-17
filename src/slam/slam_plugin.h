#ifndef ED_SLAM_PLUGIN_H_
#define ED_SLAM_PLUGIN_H_

#include <ed/plugin.h>

#include <geolib/datatypes.h>
#include <geolib/sensors/LaserRangeFinder.h>

// ROS
#include <ros/subscriber.h>
#include <ros/publisher.h>
#include <ros/callback_queue.h>
#include <sensor_msgs/LaserScan.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>

// TF
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>

class SLAMPlugin : public ed::Plugin
{

public:

    SLAMPlugin();

    virtual ~SLAMPlugin();

    void initialize(ed::InitData& init) {}

    void process(const ed::PluginInput& data, ed::UpdateRequest& req) {}

private:


    // ROS

    ros::CallbackQueue cb_queue_;

    ros::Subscriber sub_laser_;

    sensor_msgs::LaserScanConstPtr laser_msg_;

    void laserCallback(const sensor_msgs::LaserScanConstPtr& msg);

    bool have_previous_pose_;
    geo::Pose3D previous_pose_;

    ros::Subscriber sub_initial_pose_;

    geometry_msgs::PoseWithCovarianceStampedConstPtr initial_pose_msg_;

    void initialPoseCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr& msg);

    ros::Publisher pub_particles_;

    bool laser_offset_initialized_;


    // TF

    std::string map_frame_id_;
    std::string odom_frame_id_;
    std::string base_link_frame_id_;

    tf::TransformListener* tf_listener_;
    tf::TransformBroadcaster* tf_broadcaster_;

};

#endif