#include "slam_plugin.h"

#include <ros/node_handle.h>
#include <ros/subscribe_options.h>

#include <ed/world_model.h>
#include <ed/entity.h>

#include <opencv2/highgui/highgui.hpp>

#include <tue/profiling/timer.h>

#include <geolib/ros/msg_conversions.h>
#include <geolib/ros/tf_conversions.h>

#include <geometry_msgs/PoseArray.h>

// ----------------------------------------------------------------------------------------------------

SLAMPlugin::SLAMPlugin() : have_previous_pose_(false), laser_offset_initialized_(false),
    tf_listener_(), tf_broadcaster_(0)
{
}

// ----------------------------------------------------------------------------------------------------

SLAMPlugin::~SLAMPlugin()
{
}

// ----------------------------------------------------------------------------------------------------

void SLAMPlugin::initialize(ed::InitData& init)
{
    tue::Configuration& config = init.config;

    std::string laser_topic;

    if (config.readGroup("odom_model", tue::REQUIRED))
    {
        config.value("map_frame", map_frame_id_);
        config.value("odom_frame", odom_frame_id_);
        config.value("base_link_frame", base_link_frame_id_);

        odom_model_.configure(config);
        config.endGroup();
    }

    if (config.readGroup("laser_model", tue::REQUIRED))
    {
        config.value("topic", laser_topic);
        laser_model_.configure(config);
        config.endGroup();
    }

    config.value("num_particles", num_particles_);

    if (config.hasError())
        return;

    ros::NodeHandle nh;

    // Subscribe to laser topic
    ros::SubscribeOptions sub_options =
            ros::SubscribeOptions::create<sensor_msgs::LaserScan>(
                laser_topic, 1, boost::bind(&LocalizationPlugin::laserCallback, this, _1), ros::VoidPtr(), &cb_queue_);
    sub_laser_ = nh.subscribe(sub_options);

    std::string initial_pose_topic;
    if (config.value("initial_pose_topic", initial_pose_topic, tue::OPTIONAL))
    {
        // Subscribe to initial pose topic

        ros::SubscribeOptions sub_opts =
                ros::SubscribeOptions::create<geometry_msgs::PoseWithCovarianceStamped>(
                    initial_pose_topic, 1, boost::bind(&LocalizationPlugin::initialPoseCallback, this, _1), ros::VoidPtr(), &cb_queue_);
        sub_initial_pose_ = nh.subscribe(sub_opts);
    }

    if (config.readGroup("initial_pose", tue::OPTIONAL))
    {
        geo::Vec2 p;
        double yaw;

        config.value("x", p.x);
        config.value("y", p.y);
        config.value("rz", yaw);

        particle_filter_.initUniform(p - geo::Vec2(0.3, 0.3), p + geo::Vec2(0.3, 0.3), 0.05,
                                     yaw - 0.1, yaw + 0.1, 0.05);

        config.endGroup();
    }

    delete tf_listener_;
    tf_listener_ = new tf::TransformListener;

    delete tf_broadcaster_;
    tf_broadcaster_ = new tf::TransformBroadcaster;

    pub_particles_ = nh.advertise<geometry_msgs::PoseArray>("ed/localization/particles", 10);
}

// ----------------------------------------------------------------------------------------------------

void SLAMPlugin::process(const ed::PluginInput& data, ed::UpdateRequest &req)
{    
    const ed::WorldModel& world = data.world;

    laser_msg_.reset();
    initial_pose_msg_.reset();
    cb_queue_.callAvailable();

    if (initial_pose_msg_)
    {
        // Set initial pose

        geo::Vec2 p(initial_pose_msg_->pose.pose.position.x, initial_pose_msg_->pose.pose.position.y);

        double yaw = tf::getYaw(initial_pose_msg_->pose.pose.orientation);

        particle_filter_.initUniform(p - geo::Vec2(0.3, 0.3), p + geo::Vec2(0.3, 0.3), 0.05,
                                     yaw - 0.1, yaw + 0.1, 0.05);
    }

    if (!laser_msg_)
        return;

    if (!laser_offset_initialized_)
    {
        if (!tf_listener_->waitForTransform(base_link_frame_id_, laser_msg_->header.frame_id, laser_msg_->header.stamp, ros::Duration(1.0)))
        {
            ROS_WARN_STREAM("[ED LOCALIZATION] Cannot get transform from '" << base_link_frame_id_ << "' to '" << laser_msg_->header.frame_id << "'.");
            return;
        }

        try
        {
            tf::StampedTransform p_laser;
            tf_listener_->lookupTransform(base_link_frame_id_, laser_msg_->header.frame_id, laser_msg_->header.stamp, p_laser);

            geo::Transform2 offset(geo::Mat2(p_laser.getBasis()[0][0], p_laser.getBasis()[0][1],
                                             p_laser.getBasis()[1][0], p_laser.getBasis()[1][1]),
                                   geo::Vec2(p_laser.getOrigin().getX(), p_laser.getOrigin().getY()));

            double laser_height = p_laser.getOrigin().getZ();

            laser_model_.setLaserOffset(offset, laser_height);
        }
        catch (tf::TransformException e)
        {
            std::cout << "[ED LOCALIZATION] " << e.what() << std::endl;
            return;
        }

        laser_offset_initialized_ = true;
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Calculate delta movement based on odom (fetched from TF)
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    if (!tf_listener_->waitForTransform(odom_frame_id_, base_link_frame_id_, laser_msg_->header.stamp, ros::Duration(1.0)))
    {
        ROS_WARN_STREAM("[ED LOCALIZATION] Cannot get transform from '" << odom_frame_id_ << "' to '" << base_link_frame_id_ << "'.");
        return;
    }

    geo::Pose3D odom_to_base_link;
    Transform movement;

    try
    {
        tf::StampedTransform odom_to_base_link_tf;

        tf_listener_->lookupTransform(odom_frame_id_, base_link_frame_id_, laser_msg_->header.stamp, odom_to_base_link_tf);

        geo::convert(odom_to_base_link_tf, odom_to_base_link);

        if (have_previous_pose_)
        {
            geo::Pose3D delta = previous_pose_.inverse() * odom_to_base_link;

            // Convert to 2D transformation
            geo::Transform2 delta_2d(geo::Mat2(delta.R.xx, delta.R.xy,
                                               delta.R.yx, delta.R.yy),
                                     geo::Vec2(delta.t.x, delta.t.y));

            movement.set(delta_2d);
        }
        else
        {
            movement.set(geo::Transform2::identity());
        }

        previous_pose_ = odom_to_base_link;
        have_previous_pose_ = true;
    }
    catch (tf::TransformException e)
    {
        std::cout << "[ED LOCALIZATION] " << e.what() << std::endl;

        if (!have_previous_pose_)
            return;

        odom_to_base_link = previous_pose_;

        movement.set(geo::Transform2::identity());
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Check if particle filter is initialized
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    if (particle_filter_.samples().empty())
        return;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Update motion
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    odom_model_.updatePoses(movement, 0, particle_filter_);

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Update sensor
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

//    tue::Timer timer;
//    timer.start();

    laser_model_.updateWeights(world, *laser_msg_, particle_filter_);

//    std::cout << "----------" << std::endl;
//    std::cout << "Number of lines = " << laser_model_.lines_start().size() << std::endl;
//    std::cout << "Total time = " << timer.getElapsedTimeInMilliSec() << " ms" << std::endl;
//    std::cout << "Time per sample = " << timer.getElapsedTimeInMilliSec() / particle_filter_.samples().size() << " ms" << std::endl;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     (Re)sample
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    particle_filter_.resample(num_particles_);

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Publish result
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    // Get the best pose (2D)
    geo::Transform2 mean_pose = particle_filter_.calculateMeanPose();

    // Convert best pose to 3D
    geo::Pose3D map_to_base_link;
    map_to_base_link.t = geo::Vector3(mean_pose.t.x, mean_pose.t.y, 0);
    map_to_base_link.R = geo::Matrix3(mean_pose.R.xx, mean_pose.R.xy, 0,
                                      mean_pose.R.yx, mean_pose.R.yy, 0,
                                      0     , 0     , 1);

    geo::Pose3D map_to_odom = map_to_base_link * odom_to_base_link.inverse();

    // Convert to TF transform
    tf::StampedTransform map_to_odom_tf;
    geo::convert(map_to_odom, map_to_odom_tf);

    // Set frame id's and time stamp
    map_to_odom_tf.frame_id_ = map_frame_id_;
    map_to_odom_tf.child_frame_id_ = odom_frame_id_;
    map_to_odom_tf.stamp_ = laser_msg_->header.stamp;

    // Publish TF
    tf_broadcaster_->sendTransform(map_to_odom_tf);

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Publish particles
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    const std::vector<Sample>& samples = particle_filter_.samples();
    geometry_msgs::PoseArray particles_msg;
    particles_msg.poses.resize(samples.size());
    for(unsigned int i = 0; i < samples.size(); ++i)
    {
        const geo::Transform2& p = samples[i].pose.matrix();

        geo::Pose3D pose_3d;
        pose_3d.t = geo::Vector3(p.t.x, p.t.y, 0);
        pose_3d.R = geo::Matrix3(p.R.xx, p.R.xy, 0,
                                 p.R.yx, p.R.yy, 0,
                                 0     , 0     , 1);

        geo::convert(pose_3d, particles_msg.poses[i]);
    }

    particles_msg.header.frame_id = "/map";
    particles_msg.header.stamp = laser_msg_->header.stamp;

    pub_particles_.publish(particles_msg);

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // -     Visualization
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    bool visualize = false;
    if (visualize)
    {
        int grid_size = 800;
        double grid_resolution = 0.025;

        cv::Mat rgb_image(grid_size, grid_size, CV_8UC3, cv::Scalar(10, 10, 10));

        std::vector<geo::Vector3> sensor_points;
        laser_model_.renderer().rangesToPoints(laser_model_.sensor_ranges(), sensor_points);

        geo::Transform2 best_pose = mean_pose;

        geo::Transform2 laser_pose = best_pose * laser_model_.laser_offset();
        for(unsigned int i = 0; i < sensor_points.size(); ++i)
        {
            const geo::Vec2& p = laser_pose * geo::Vec2(sensor_points[i].x, sensor_points[i].y);
            int mx = -(p.y - best_pose.t.y) / grid_resolution + grid_size / 2;
            int my = -(p.x - best_pose.t.x) / grid_resolution + grid_size / 2;

            if (mx >= 0 && my >= 0 && mx < grid_size && my <grid_size)
            {
                rgb_image.at<cv::Vec3b>(my, mx) = cv::Vec3b(0, 255, 0);
            }
        }

        const std::vector<geo::Vec2>& lines_start = laser_model_.lines_start();
        const std::vector<geo::Vec2>& lines_end = laser_model_.lines_end();

        for(unsigned int i = 0; i < lines_start.size(); ++i)
        {
            const geo::Vec2& p1 = lines_start[i];
            int mx1 = -(p1.y - best_pose.t.y) / grid_resolution + grid_size / 2;
            int my1 = -(p1.x - best_pose.t.x) / grid_resolution + grid_size / 2;

            const geo::Vec2& p2 = lines_end[i];
            int mx2 = -(p2.y - best_pose.t.y) / grid_resolution + grid_size / 2;
            int my2 = -(p2.x - best_pose.t.x) / grid_resolution + grid_size / 2;

            cv::line(rgb_image, cv::Point(mx1, my1), cv::Point(mx2, my2), cv::Scalar(255, 255, 255), 1);
        }

        const std::vector<Sample>& samples = particle_filter_.samples();
        for(std::vector<Sample>::const_iterator it = samples.begin(); it != samples.end(); ++it)
        {
            const geo::Transform2& pose = it->pose.matrix();

            // Visualize sensor
            int lmx = -(pose.t.y - best_pose.t.y) / grid_resolution + grid_size / 2;
            int lmy = -(pose.t.x - best_pose.t.x) / grid_resolution + grid_size / 2;
            cv::circle(rgb_image, cv::Point(lmx,lmy), 0.1 / grid_resolution, cv::Scalar(0, 0, 255), 1);

            geo::Vec2 d = pose.R * geo::Vec2(0.2, 0);
            int dmx = -d.y / grid_resolution;
            int dmy = -d.x / grid_resolution;
            cv::line(rgb_image, cv::Point(lmx, lmy), cv::Point(lmx + dmx, lmy + dmy), cv::Scalar(0, 0, 255), 1);
        }

        cv::imshow("localization", rgb_image);
        cv::waitKey(1);
    }
}

// ----------------------------------------------------------------------------------------------------

void SLAMPlugin::laserCallback(const sensor_msgs::LaserScanConstPtr& msg)
{
    laser_msg_ = msg;
}

// ----------------------------------------------------------------------------------------------------

void SLAMPlugin::initialPoseCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr& msg)
{
    initial_pose_msg_ = msg;
}

// ----------------------------------------------------------------------------------------------------

ED_REGISTER_PLUGIN(SLAMPlugin)