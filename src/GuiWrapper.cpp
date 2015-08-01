/*
Copyright (c) 2010-2014, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "GuiWrapper.h"
#include <QtGui/QApplication>
#include <QtCore/QDir>

#include <cv_bridge/cv_bridge.h>
#include <std_srvs/Empty.h>
#include <std_msgs/Empty.h>
#include <sensor_msgs/image_encodings.h>

#include <rtabmap/utilite/UEventsManager.h>
#include <rtabmap/utilite/UConversion.h>

#include <opencv2/highgui/highgui.hpp>

#include <image_geometry/pinhole_camera_model.h>
#include <image_geometry/stereo_camera_model.h>

#include <rtabmap/gui/MainWindow.h>
#include <rtabmap/core/RtabmapEvent.h>
#include <rtabmap/core/Parameters.h>
#include <rtabmap/core/ParamEvent.h>
#include <rtabmap/core/OdometryEvent.h>
#include <rtabmap/core/util3d.h>
#include <rtabmap/core/util3d_transforms.h>
#include <rtabmap/utilite/UTimer.h>

#include "rtabmap_ros/MsgConversion.h"
#include "rtabmap_ros/GetMap.h"
#include "rtabmap_ros/SetGoal.h"
#include "rtabmap_ros/SetLabel.h"

#include "PreferencesDialogROS.h"

#include <pcl_ros/transforms.h>
#include <pcl_conversions/pcl_conversions.h>
#include <laser_geometry/laser_geometry.h>

float max3( const float& a, const float& b, const float& c)
{
	float m=a>b?a:b;
	return m>c?m:c;
}

GuiWrapper::GuiWrapper(int & argc, char** argv) :
		app_(0),
		mainWindow_(0),
		frameId_("base_link"),
		waitForTransform_(true),
		cameraNodeName_(""),
		lastOdomInfoUpdateTime_(0),
		depthScanSync_(0),
		depthSync_(0),
		depthOdomInfoSync_(0),
		stereoSync_(0),
		stereoScanSync_(0),
		stereoOdomInfoSync_(0),
		depth2Sync_(0),
		depthOdomInfo2Sync_(0)
{
	ros::NodeHandle nh;
	app_ = new QApplication(argc, argv);

	QString configFile = QDir::homePath()+"/.ros/rtabmapGUI.ini";
	for(int i=1; i<argc; ++i)
	{
		if(strcmp(argv[i], "-d") == 0)
		{
			++i;
			if(i < argc)
			{
				configFile = argv[i];
			}
			break;
		}
	}

	configFile.replace('~', QDir::homePath());

	ROS_INFO("rtabmapviz: Using configuration from \"%s\"", configFile.toStdString().c_str());
	uSleep(500);
	mainWindow_ = new MainWindow(new PreferencesDialogROS(configFile));
	mainWindow_->setWindowTitle(mainWindow_->windowTitle()+" [ROS]");
	mainWindow_->show();
	bool paused = false;
	nh.param("is_rtabmap_paused", paused, paused);
	mainWindow_->setMonitoringState(paused);
	app_->connect( app_, SIGNAL( lastWindowClosed() ), app_, SLOT( quit() ) );

	ros::NodeHandle pnh("~");

	// To receive odometry events
	bool subscribeLaserScan = false;
	bool subscribeDepth = false;
	bool subscribeOdomInfo = false;
	bool subscribeStereo = false;
	int queueSize = 10;
	int depthCameras = 1;
	std::string tfPrefix;
	pnh.param("frame_id", frameId_, frameId_);
	pnh.param("odom_frame_id", odomFrameId_, odomFrameId_); // set to use odom from TF
	pnh.param("subscribe_depth", subscribeDepth, subscribeDepth);
	pnh.param("subscribe_laserScan", subscribeLaserScan, subscribeLaserScan);
	pnh.param("subscribe_odom_info", subscribeOdomInfo, subscribeOdomInfo);
	pnh.param("subscribe_stereo", subscribeStereo, subscribeStereo);
	pnh.param("depth_cameras", depthCameras, depthCameras);
	pnh.param("queue_size", queueSize, queueSize);
	pnh.param("tf_prefix", tfPrefix, tfPrefix);
	pnh.param("wait_for_transform", waitForTransform_, waitForTransform_);
	pnh.param("camera_node_name", cameraNodeName_, cameraNodeName_); // used to pause the rtabmap_ros/camera when pausing the process

	if(!tfPrefix.empty())
	{
		if(!frameId_.empty())
		{
			frameId_ = tfPrefix + "/" + frameId_;
		}
		if(!odomFrameId_.empty())
		{
			odomFrameId_ = tfPrefix + "/" + odomFrameId_;
		}
	}

	this->setupCallbacks(
			subscribeDepth,
			subscribeLaserScan,
			subscribeOdomInfo,
			subscribeStereo,
			queueSize,
			depthCameras);

	UEventsManager::addHandler(this);
	UEventsManager::addHandler(mainWindow_);

	infoTopic_.subscribe(nh, "info", 1);
	mapDataTopic_.subscribe(nh, "mapData", 1);
	infoMapSync_ = new message_filters::Synchronizer<MyInfoMapSyncPolicy>(
			MyInfoMapSyncPolicy(queueSize),
			infoTopic_,
			mapDataTopic_);
	infoMapSync_->registerCallback(boost::bind(&GuiWrapper::infoMapCallback, this, _1, _2));
}

GuiWrapper::~GuiWrapper()
{
	if(depthSync_)
		delete depthSync_;
	if(depth2Sync_)
		delete depth2Sync_;
	if(depthScanSync_)
		delete depthScanSync_;
	if(depthOdomInfoSync_)
		delete depthOdomInfoSync_;
	if(depthOdomInfo2Sync_)
		delete depthOdomInfo2Sync_;
	if(stereoSync_)
		delete stereoSync_;
	if(stereoScanSync_)
		delete stereoScanSync_;
	if(stereoOdomInfoSync_)
		delete stereoOdomInfoSync_;

	for(unsigned int i=0; i<imageSubs_.size(); ++i)
	{
		delete imageSubs_[i];
	}
	imageSubs_.clear();
	for(unsigned int i=0; i<imageDepthSubs_.size(); ++i)
	{
		delete imageDepthSubs_[i];
	}
	imageDepthSubs_.clear();
	for(unsigned int i=0; i<cameraInfoSubs_.size(); ++i)
	{
		delete cameraInfoSubs_[i];
	}
	cameraInfoSubs_.clear();

	delete infoMapSync_;
	delete mainWindow_;
	delete app_;
}

int GuiWrapper::exec()
{
	return app_->exec();
}

void GuiWrapper::infoMapCallback(
		const rtabmap_ros::InfoConstPtr & infoMsg,
		const rtabmap_ros::MapDataConstPtr & mapMsg)
{
	//ROS_INFO("rtabmapviz: RTAB-Map info ex received!");

	// Map from ROS struct to rtabmap struct
	rtabmap::Statistics stat;

	// Info
	rtabmap_ros::infoFromROS(*infoMsg, stat);

	// MapData
	rtabmap::Transform mapToOdom;
	std::map<int, rtabmap::Transform> poses;
	std::map<int, Signature> signatures;
	std::multimap<int, Link> links;

	rtabmap_ros::mapDataFromROS(*mapMsg, poses, links, signatures, mapToOdom);

	stat.setMapCorrection(mapToOdom);
	stat.setPoses(poses);
	stat.setSignatures(signatures);
	stat.setConstraints(links);

	this->post(new RtabmapEvent(stat));
}

void GuiWrapper::processRequestedMap(const rtabmap_ros::MapData & map)
{
	std::map<int, Signature> signatures;
	std::map<int, Transform> poses;
	std::multimap<int, rtabmap::Link> constraints;
	Transform mapToOdom;

	rtabmap_ros::mapDataFromROS(map, poses, constraints, signatures, mapToOdom);

	RtabmapEvent3DMap e(signatures,
				poses,
				constraints);
	QMetaObject::invokeMethod(mainWindow_, "processRtabmapEvent3DMap", Q_ARG(rtabmap::RtabmapEvent3DMap, e));
}

void GuiWrapper::handleEvent(UEvent * anEvent)
{
	if(anEvent->getClassName().compare("ParamEvent") == 0)
	{
		const rtabmap::ParametersMap & defaultParameters = rtabmap::Parameters::getDefaultParameters();
		rtabmap::ParametersMap parameters = ((rtabmap::ParamEvent *)anEvent)->getParameters();
		bool modified = false;
		ros::NodeHandle nh;
		for(rtabmap::ParametersMap::iterator i=parameters.begin(); i!=parameters.end(); ++i)
		{
			//save only parameters with valid names
			if(defaultParameters.find((*i).first) != defaultParameters.end())
			{
				nh.setParam((*i).first, (*i).second);
				modified = true;
			}
			else if((*i).first.find('/') != (*i).first.npos)
			{
				ROS_WARN("Parameter %s is not used by the rtabmap node.", (*i).first.c_str());
			}
		}
		if(modified)
		{
			ROS_INFO("Parameters updated");
			std_srvs::Empty srv;
			if(!ros::service::call("update_parameters", srv))
			{
				ROS_ERROR("Can't call \"update_parameters\" service");
			}
		}
	}
	else if(anEvent->getClassName().compare("RtabmapEventCmd") == 0)
	{
		std_srvs::Empty emptySrv;
		rtabmap::RtabmapEventCmd * cmdEvent = (rtabmap::RtabmapEventCmd *)anEvent;
		rtabmap::RtabmapEventCmd::Cmd cmd = cmdEvent->getCmd();
		if(cmd == rtabmap::RtabmapEventCmd::kCmdResetMemory)
		{
			if(!ros::service::call("reset", emptySrv))
			{
				ROS_ERROR("Can't call \"reset\" service");
			}
		}
		else if(cmd == rtabmap::RtabmapEventCmd::kCmdPause)
		{
			if(cmdEvent->getInt())
			{
				// Pause the camera if the rtabmap/camera node is used
				if(!cameraNodeName_.empty())
				{
					std::string str = uFormat("rosrun dynamic_reconfigure dynparam set %s pause true", cameraNodeName_.c_str());
					system(str.c_str());
				}

				// Pause visual_odometry
				ros::service::call("pause_odom", emptySrv);

				// Pause rtabmap
				if(!ros::service::call("pause", emptySrv))
				{
					ROS_ERROR("Can't call \"pause\" service");
				}
			}
			else
			{
				// Resume rtabmap
				if(!ros::service::call("resume", emptySrv))
				{
					ROS_ERROR("Can't call \"resume\" service");
				}

				// Pause visual_odometry
				ros::service::call("resume_odom", emptySrv);

				// Resume the camera if the rtabmap/camera node is used
				if(!cameraNodeName_.empty())
				{
					std::string str = uFormat("rosrun dynamic_reconfigure dynparam set %s pause false", cameraNodeName_.c_str());
					system(str.c_str());
				}
			}
		}
		else if(cmd == rtabmap::RtabmapEventCmd::kCmdTriggerNewMap)
		{
			if(!ros::service::call("trigger_new_map", emptySrv))
			{
				ROS_ERROR("Can't call \"trigger_new_map\" service");
			}
		}
		else if(cmd == rtabmap::RtabmapEventCmd::kCmdPublish3DMapLocal ||
				 cmd == rtabmap::RtabmapEventCmd::kCmdPublish3DMapGlobal ||
				 cmd == rtabmap::RtabmapEventCmd::kCmdPublishTOROGraphLocal ||
				 cmd == rtabmap::RtabmapEventCmd::kCmdPublishTOROGraphGlobal)
		{
			rtabmap_ros::GetMap getMapSrv;
			getMapSrv.request.global = cmd == rtabmap::RtabmapEventCmd::kCmdPublish3DMapGlobal || cmd == rtabmap::RtabmapEventCmd::kCmdPublishTOROGraphGlobal;
			getMapSrv.request.optimized = cmdEvent->getInt();
			getMapSrv.request.graphOnly = cmd == rtabmap::RtabmapEventCmd::kCmdPublishTOROGraphGlobal || cmd == rtabmap::RtabmapEventCmd::kCmdPublishTOROGraphLocal;
			if(!ros::service::call("get_map", getMapSrv))
			{
				ROS_WARN("Can't call \"get_map\" service");
				this->post(new RtabmapEvent3DMap(1)); // service error
			}
			else
			{
				processRequestedMap(getMapSrv.response.data);
			}
		}
		else if(cmd == rtabmap::RtabmapEventCmd::kCmdGoal)
		{
			rtabmap_ros::SetGoal setGoalSrv;
			setGoalSrv.request.node_id = cmdEvent->getInt();
			setGoalSrv.request.node_label = cmdEvent->getStr();
			if(!ros::service::call("set_goal", setGoalSrv))
			{
				ROS_ERROR("Can't call \"set_goal\" service");
			}
		}
		else if(cmd == rtabmap::RtabmapEventCmd::kCmdCancelGoal)
		{
			if(!ros::service::call("cancel_goal", emptySrv))
			{
				ROS_ERROR("Can't call \"cancel_goal\" service");
			}
		}
		else if(cmd == rtabmap::RtabmapEventCmd::kCmdLabel)
		{
			rtabmap_ros::SetLabel setLabelSrv;
			setLabelSrv.request.node_id = cmdEvent->getInt();
			setLabelSrv.request.node_label = cmdEvent->getStr();
			if(!ros::service::call("set_label", setLabelSrv))
			{
				ROS_ERROR("Can't call \"set_label\" service");
			}
		}
		else
		{
			ROS_WARN("Not handled command (%d)...", cmd);
		}
	}
	else if(anEvent->getClassName().compare("OdometryResetEvent") == 0)
	{
		std_srvs::Empty srv;
		if(!ros::service::call("reset_odom", srv))
		{
			ROS_ERROR("Can't call \"reset_odom\" service, (will only work with rtabmap/visual_odometry node.)");
		}
	}
}

Transform GuiWrapper::getTransform(const std::string & fromFrameId, const std::string & toFrameId, const ros::Time & stamp) const
{
	// TF ready?
	Transform localTransform;
	try
	{
		if(waitForTransform_ && !stamp.isZero())
		{
			//if(!tfBuffer_.canTransform(fromFrameId, toFrameId, stamp, ros::Duration(1)))
			if(!tfListener_.waitForTransform(fromFrameId, toFrameId, stamp, ros::Duration(1)))
			{
				ROS_WARN("Could not get transform from %s to %s after 1 second!", fromFrameId.c_str(), toFrameId.c_str());
				return localTransform;
			}
		}

		tf::StampedTransform tmp;
		tfListener_.lookupTransform(fromFrameId, toFrameId, stamp, tmp);
		localTransform = rtabmap_ros::transformFromTF(tmp);
	}
	catch(tf::TransformException & ex)
	{
		ROS_WARN("%s",ex.what());
	}
	return localTransform;
}

void GuiWrapper::commonDepthCallback(
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg,
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const rtabmap_ros::OdomInfoConstPtr& odomInfoMsg)
{
	std::vector<sensor_msgs::ImageConstPtr> imageMsgs;
	std::vector<sensor_msgs::ImageConstPtr> depthMsgs;
	std::vector<sensor_msgs::CameraInfoConstPtr> cameraInfoMsgs;
	imageMsgs.push_back(imageMsg);
	depthMsgs.push_back(depthMsg);
	cameraInfoMsgs.push_back(cameraInfoMsg);
	commonDepthCallback(odomMsg, imageMsgs, depthMsgs, cameraInfoMsgs, scanMsg, odomInfoMsg);
}

void GuiWrapper::commonDepthCallback(
		const nav_msgs::OdometryConstPtr & odomMsg,
		const std::vector<sensor_msgs::ImageConstPtr> & imageMsgs,
		const std::vector<sensor_msgs::ImageConstPtr> & depthMsgs,
		const std::vector<sensor_msgs::CameraInfoConstPtr> & cameraInfoMsgs,
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const rtabmap_ros::OdomInfoConstPtr& odomInfoMsg)
{
	if(UTimer::now() - lastOdomInfoUpdateTime_ > 0.1 &&
	   !mainWindow_->isProcessingOdometry() &&
	   !mainWindow_->isProcessingStatistics())
	{
		lastOdomInfoUpdateTime_ = UTimer::now();

		UASSERT(imageMsgs.size()>0 &&
				imageMsgs.size() == depthMsgs.size() &&
				imageMsgs.size() == cameraInfoMsgs.size());

		std_msgs::Header odomHeader;
		if(odomMsg.get())
		{
			odomHeader = odomMsg->header;
		}
		else
		{
			if(scanMsg.get())
			{
				odomHeader = scanMsg->header;
			}
			else if(cameraInfoMsgs.size() && cameraInfoMsgs[0].get())
			{
				odomHeader = cameraInfoMsgs[0]->header;
			}
			else if(depthMsgs.size() && depthMsgs[0].get())
			{
				odomHeader = depthMsgs[0]->header;
			}
			else if(imageMsgs.size() && imageMsgs[0].get())
			{
				odomHeader = imageMsgs[0]->header;
			}
			odomHeader.frame_id = odomFrameId_;
		}

		Transform odomT = getTransform(odomHeader.frame_id, frameId_, odomHeader.stamp);
		cv::Mat covariance = cv::Mat::eye(6,6,CV_64FC1);
		if(odomMsg.get())
		{
			UASSERT(odomMsg->pose.covariance.size() == 36);
			if(!(odomMsg->pose.covariance[0] == 0 &&
				 odomMsg->pose.covariance[7] == 0 &&
				 odomMsg->pose.covariance[14] == 0 &&
				 odomMsg->pose.covariance[21] == 0 &&
				 odomMsg->pose.covariance[28] == 0 &&
				 odomMsg->pose.covariance[35] == 0))
			{
				covariance = cv::Mat(6,6,CV_64FC1,(void*)odomMsg->pose.covariance.data()).clone();
			}
		}
		if(odomHeader.frame_id.empty())
		{
			ROS_ERROR("Odometry frame not set!?");
			return;
		}

		//for sync transform
		if(odomT.isNull())
		{
			return;
		}

		int imageWidth = imageMsgs[0]->width;
		int imageHeight = imageMsgs[0]->height;
		int cameraCount = imageMsgs.size();
		cv::Mat rgb;
		cv::Mat depth;
		pcl::PointCloud<pcl::PointXYZ> scanCloud;
		std::vector<CameraModel> cameraModels;
		for(unsigned int i=0; i<imageMsgs.size(); ++i)
		{
			if(!(imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_8UC1) ==0 ||
				 imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::MONO8) ==0 ||
				 imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::MONO16) ==0 ||
				 imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::BGR8) == 0 ||
				 imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::RGB8) == 0) ||
				!(depthMsgs[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_16UC1) == 0 ||
				 depthMsgs[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_32FC1) == 0 ||
				 depthMsgs[i]->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0))
			{
				ROS_ERROR("Input type must be image=mono8,mono16,rgb8,bgr8 and image_depth=32FC1,16UC1,mono16");
				return;
			}
			UASSERT(imageMsgs[i]->width == imageWidth && imageMsgs[i]->height == imageHeight);
			UASSERT(depthMsgs[i]->width == imageWidth && depthMsgs[i]->height == imageHeight);

			Transform localTransform = getTransform(frameId_, depthMsgs[i]->header.frame_id, depthMsgs[i]->header.stamp);
			if(localTransform.isNull())
			{
				return;
			}
			// sync with odometry stamp
			if(odomHeader.stamp != depthMsgs[i]->header.stamp)
			{
				if(!odomT.isNull())
				{
					Transform sensorT = getTransform(odomHeader.frame_id, frameId_, depthMsgs[i]->header.stamp);
					if(sensorT.isNull())
					{
						return;
					}
					localTransform = odomT.inverse() * sensorT * localTransform;
				}
			}

			cv_bridge::CvImageConstPtr ptrImage;
			if(imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::TYPE_8UC1)==0)
			{
				ptrImage = cv_bridge::toCvShare(imageMsgs[i]);
			}
			else if(imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::MONO8) == 0 ||
			   imageMsgs[i]->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0)
			{
				ptrImage = cv_bridge::toCvShare(imageMsgs[i], "mono8");
			}
			else
			{
				ptrImage = cv_bridge::toCvShare(imageMsgs[i], "bgr8");
			}
			cv_bridge::CvImageConstPtr ptrDepth = cv_bridge::toCvShare(depthMsgs[i]);
			cv::Mat subDepth = ptrDepth->image;
			if(subDepth.type() == CV_32FC1)
			{
				subDepth = util3d::cvtDepthFromFloat(subDepth);
				static bool shown = false;
				if(!shown)
				{
					ROS_WARN("Use depth image with \"unsigned short\" type to "
							 "avoid conversion. This message is only printed once...");
					shown = true;
				}
			}

			// initialize
			if(rgb.empty())
			{
				rgb = cv::Mat(imageHeight, imageWidth*cameraCount, ptrImage->image.type());
			}
			if(depth.empty())
			{
				depth = cv::Mat(imageHeight, imageWidth*cameraCount, subDepth.type());
			}

			if(ptrImage->image.type() == rgb.type())
			{
				ptrImage->image.copyTo(cv::Mat(rgb, cv::Rect(i*imageWidth, 0, imageWidth, imageHeight)));
			}
			else
			{
				ROS_ERROR("Some RGB images are not the same type!");
				return;
			}

			if(subDepth.type() == depth.type())
			{
				subDepth.copyTo(cv::Mat(depth, cv::Rect(i*imageWidth, 0, imageWidth, imageHeight)));
			}
			else
			{
				ROS_ERROR("Some Depth images are not the same type!");
				return;
			}

			image_geometry::PinholeCameraModel model;
			model.fromCameraInfo(*cameraInfoMsgs[i]);
			cameraModels.push_back(rtabmap::CameraModel(
					model.fx(),
					model.fy(),
					model.cx(),
					model.cy(),
					localTransform));
		}

		cv::Mat scan;
		if(scanMsg.get() != 0)
		{
			// make sure the frame of the laser is updated too
			if(getTransform(frameId_, scanMsg->header.frame_id, scanMsg->header.stamp).isNull())
			{
				return;
			}

			//transform in frameId_ frame
			sensor_msgs::PointCloud2 scanOut;
			laser_geometry::LaserProjection projection;
			projection.transformLaserScanToPointCloud(frameId_, *scanMsg, scanOut, tfListener_);
			pcl::PointCloud<pcl::PointXYZ>::Ptr pclScan(new pcl::PointCloud<pcl::PointXYZ>);
			pcl::fromROSMsg(scanOut, *pclScan);

			// sync with odometry stamp
			if(odomHeader.stamp != scanMsg->header.stamp)
			{
				if(!odomT.isNull())
				{
					Transform sensorT = getTransform(odomHeader.frame_id, frameId_, scanMsg->header.stamp);
					if(sensorT.isNull())
					{
						return;
					}
					Transform t = odomT.inverse() * sensorT;
					pclScan = util3d::transformPointCloud(pclScan, t);

				}
			}
			scan = util3d::laserScanFromPointCloud(*pclScan);
		}

		rtabmap::OdometryInfo info;
		if(odomInfoMsg.get())
		{
			info = rtabmap_ros::odomInfoFromROS(*odomInfoMsg);
		}

		rtabmap::OdometryEvent odomEvent(
			rtabmap::SensorData(
					scan,
					scanMsg.get()?(int)scanMsg->ranges.size():0,
					rgb,
					depth,
					cameraModels,
					odomHeader.seq,
					rtabmap_ros::timestampFromROS(odomHeader.stamp)),
			odomT,
			covariance,
			info);

		QMetaObject::invokeMethod(mainWindow_, "processOdometry", Q_ARG(rtabmap::OdometryEvent, odomEvent));
	}
}

void GuiWrapper::commonStereoCallback(
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCamInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCamInfoMsg,
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const rtabmap_ros::OdomInfoConstPtr& odomInfoMsg)
{
	// limit 10 Hz max
	if(UTimer::now() - lastOdomInfoUpdateTime_ > 0.1 &&
	   !mainWindow_->isProcessingOdometry() &&
	   !mainWindow_->isProcessingStatistics())
	{
		lastOdomInfoUpdateTime_ = UTimer::now();

		UASSERT(leftImageMsg.get() && rightImageMsg.get());
		UASSERT(leftCamInfoMsg.get() && rightCamInfoMsg.get());

		if(!(leftImageMsg->encoding.compare(sensor_msgs::image_encodings::MONO8) == 0 ||
			 leftImageMsg->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0 ||
			 leftImageMsg->encoding.compare(sensor_msgs::image_encodings::BGR8) == 0 ||
			 leftImageMsg->encoding.compare(sensor_msgs::image_encodings::RGB8) == 0) ||
		   !(rightImageMsg->encoding.compare(sensor_msgs::image_encodings::MONO8) == 0 ||
			 rightImageMsg->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0 ||
			 rightImageMsg->encoding.compare(sensor_msgs::image_encodings::BGR8) == 0 ||
			 rightImageMsg->encoding.compare(sensor_msgs::image_encodings::RGB8) == 0))
		{
			ROS_ERROR("Input type must be image=mono8,mono16,rgb8,bgr8");
			return;
		}

		std_msgs::Header odomHeader;
		if(odomMsg.get())
		{
			odomHeader = odomMsg->header;
		}
		else
		{
			if(scanMsg.get())
			{
				odomHeader = scanMsg->header;
			}
			else
			{
				odomHeader = leftCamInfoMsg->header;
			}
			odomHeader.frame_id = odomFrameId_;
		}

		Transform odomT = getTransform(odomHeader.frame_id, frameId_, odomHeader.stamp);
		cv::Mat covariance = cv::Mat::eye(6,6,CV_64FC1);
		if(odomMsg.get())
		{
			UASSERT(odomMsg->pose.covariance.size() == 36);
			if(!(odomMsg->pose.covariance[0] == 0 &&
				 odomMsg->pose.covariance[7] == 0 &&
				 odomMsg->pose.covariance[14] == 0 &&
				 odomMsg->pose.covariance[21] == 0 &&
				 odomMsg->pose.covariance[28] == 0 &&
				 odomMsg->pose.covariance[35] == 0))
			{
				covariance = cv::Mat(6,6,CV_64FC1,(void*)odomMsg->pose.covariance.data()).clone();
			}
		}
		if(odomHeader.frame_id.empty())
		{
			ROS_ERROR("Odometry frame not set!?");
			return;
		}

		//for sync transform
		if(odomT.isNull())
		{
			return;
		}

		Transform localTransform = getTransform(frameId_, leftCamInfoMsg->header.frame_id, leftCamInfoMsg->header.stamp);
		if(localTransform.isNull())
		{
			return;
		}
		// sync with odometry stamp
		if(odomHeader.stamp != leftCamInfoMsg->header.stamp)
		{
			Transform sensorT = getTransform(odomHeader.frame_id, frameId_, leftCamInfoMsg->header.stamp);
			if(sensorT.isNull())
			{
				return;
			}
			localTransform = odomT.inverse() * sensorT * localTransform;
		}

		image_geometry::StereoCameraModel model;
		model.fromCameraInfo(*leftCamInfoMsg, *rightCamInfoMsg);
		rtabmap::StereoCameraModel stereoModel(
				model.left().fx(),
				model.left().fy(),
				model.left().cx(),
				model.left().cy(),
				model.baseline(),
				localTransform);

		// left
		cv_bridge::CvImageConstPtr ptrImage;
		cv::Mat left;
		if(leftImageMsg->encoding.compare(sensor_msgs::image_encodings::MONO8) == 0 ||
		   leftImageMsg->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0)
		{
			left = cv_bridge::toCvCopy(leftImageMsg, "mono8")->image;
		}
		else
		{
			left = cv_bridge::toCvCopy(leftImageMsg, "bgr8")->image;
		}

		// right
		cv::Mat right = cv_bridge::toCvCopy(rightImageMsg, "mono8")->image;

		cv::Mat scan;
		if(scanMsg.get() != 0)
		{
			// make sure the frame of the laser is updated too
			if(getTransform(frameId_, scanMsg->header.frame_id, scanMsg->header.stamp).isNull())
			{
				return;
			}

			//transform in frameId_ frame
			sensor_msgs::PointCloud2 scanOut;
			laser_geometry::LaserProjection projection;
			projection.transformLaserScanToPointCloud(frameId_, *scanMsg, scanOut, tfListener_);
			pcl::PointCloud<pcl::PointXYZ>::Ptr pclScan(new pcl::PointCloud<pcl::PointXYZ>);
			pcl::fromROSMsg(scanOut, *pclScan);

			// sync with odometry stamp
			if(odomHeader.stamp != scanMsg->header.stamp)
			{
				if(!odomT.isNull())
				{
					Transform sensorT = getTransform(odomHeader.frame_id, frameId_, scanMsg->header.stamp);
					if(sensorT.isNull())
					{
						return;
					}
					Transform t = odomT.inverse() * sensorT;
					pclScan = util3d::transformPointCloud(pclScan, t);

				}
			}
			scan = util3d::laserScanFromPointCloud(*pclScan);
		}

		rtabmap::OdometryInfo info;
		if(odomInfoMsg.get())
		{
			info = rtabmap_ros::odomInfoFromROS(*odomInfoMsg);
		}

		rtabmap::OdometryEvent odomEvent(
			rtabmap::SensorData(
					scan,
					scanMsg.get()?(int)scanMsg->ranges.size():0,
					left,
					right,
					stereoModel,
					odomHeader.seq,
					rtabmap_ros::timestampFromROS(odomHeader.stamp)),
			odomT,
			covariance,
			info);

		QMetaObject::invokeMethod(mainWindow_, "processOdometry", Q_ARG(rtabmap::OdometryEvent, odomEvent));
	}
}

// With odom msg
void GuiWrapper::defaultCallback(const nav_msgs::OdometryConstPtr & odomMsg)
{
	commonDepthCallback(
			odomMsg,
			sensor_msgs::ImageConstPtr(),
			sensor_msgs::ImageConstPtr(),
			sensor_msgs::CameraInfoConstPtr(),
			sensor_msgs::LaserScanConstPtr(),
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::depthCallback(
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	commonDepthCallback(
			odomMsg,
			imageMsg,
			depthMsg,
			cameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::depth2Callback(
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& image1Msg,
		const sensor_msgs::ImageConstPtr& depth1Msg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfo1Msg,
		const sensor_msgs::ImageConstPtr& image2Msg,
		const sensor_msgs::ImageConstPtr& depth2Msg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfo2Msg)
{
	std::vector<sensor_msgs::ImageConstPtr> imageMsgs;
	std::vector<sensor_msgs::ImageConstPtr> depthMsgs;
	std::vector<sensor_msgs::CameraInfoConstPtr> cameraInfoMsgs;
	imageMsgs.push_back(image1Msg);
	imageMsgs.push_back(image2Msg);
	depthMsgs.push_back(depth1Msg);
	depthMsgs.push_back(depth2Msg);
	cameraInfoMsgs.push_back(cameraInfo1Msg);
	cameraInfoMsgs.push_back(cameraInfo2Msg);

	commonDepthCallback(
			odomMsg,
			imageMsgs,
			depthMsgs,
			cameraInfoMsgs,
			sensor_msgs::LaserScanConstPtr(),
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::depthOdomInfoCallback(
		const rtabmap_ros::OdomInfoConstPtr & odomInfoMsg,
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	commonDepthCallback(
			odomMsg,
			imageMsg,
			depthMsg,
			cameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			odomInfoMsg);
}

void GuiWrapper::depthOdomInfo2Callback(
		const rtabmap_ros::OdomInfoConstPtr & odomInfoMsg,
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& image1Msg,
		const sensor_msgs::ImageConstPtr& depth1Msg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfo1Msg,
		const sensor_msgs::ImageConstPtr& image2Msg,
		const sensor_msgs::ImageConstPtr& depth2Msg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfo2Msg)
{
	std::vector<sensor_msgs::ImageConstPtr> imageMsgs;
	std::vector<sensor_msgs::ImageConstPtr> depthMsgs;
	std::vector<sensor_msgs::CameraInfoConstPtr> cameraInfoMsgs;
	imageMsgs.push_back(image1Msg);
	imageMsgs.push_back(image2Msg);
	depthMsgs.push_back(depth1Msg);
	depthMsgs.push_back(depth2Msg);
	cameraInfoMsgs.push_back(cameraInfo1Msg);
	cameraInfoMsgs.push_back(cameraInfo2Msg);

	commonDepthCallback(
			odomMsg,
			imageMsgs,
			depthMsgs,
			cameraInfoMsgs,
			sensor_msgs::LaserScanConstPtr(),
			odomInfoMsg);
}

void GuiWrapper::depthScanCallback(
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	commonDepthCallback(
			odomMsg,
			imageMsg,
			depthMsg,
			cameraInfoMsg,
			scanMsg,
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::stereoScanCallback(
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCameraInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCameraInfoMsg)
{
	commonStereoCallback(
			odomMsg,
			leftImageMsg,
			rightImageMsg,
			leftCameraInfoMsg,
			rightCameraInfoMsg,
			scanMsg,
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::stereoOdomInfoCallback(
		const rtabmap_ros::OdomInfoConstPtr & odomInfoMsg,
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCameraInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCameraInfoMsg)
{
	commonStereoCallback(
			odomMsg,
			leftImageMsg,
			rightImageMsg,
			leftCameraInfoMsg,
			rightCameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			odomInfoMsg);
}

void GuiWrapper::stereoCallback(
		const nav_msgs::OdometryConstPtr & odomMsg,
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCameraInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCameraInfoMsg)
{
	commonStereoCallback(
			odomMsg,
			leftImageMsg,
			rightImageMsg,
			leftCameraInfoMsg,
			rightCameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			rtabmap_ros::OdomInfoConstPtr());
}

// With odom TF
void GuiWrapper::depthTFCallback(
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	commonDepthCallback(
			nav_msgs::OdometryConstPtr(),
			imageMsg,
			depthMsg,
			cameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::depthOdomInfoTFCallback(
		const rtabmap_ros::OdomInfoConstPtr & odomInfoMsg,
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	commonDepthCallback(
			nav_msgs::OdometryConstPtr(),
			imageMsg,
			depthMsg,
			cameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			odomInfoMsg);
}

void GuiWrapper::depthScanTFCallback(
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const sensor_msgs::ImageConstPtr& imageMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	commonDepthCallback(
			nav_msgs::OdometryConstPtr(),
			imageMsg,
			depthMsg,
			cameraInfoMsg,
			scanMsg,
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::stereoScanTFCallback(
		const sensor_msgs::LaserScanConstPtr& scanMsg,
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCameraInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCameraInfoMsg)
{
	commonStereoCallback(
			nav_msgs::OdometryConstPtr(),
			leftImageMsg,
			rightImageMsg,
			leftCameraInfoMsg,
			rightCameraInfoMsg,
			scanMsg,
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::stereoOdomInfoTFCallback(
		const rtabmap_ros::OdomInfoConstPtr & odomInfoMsg,
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCameraInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCameraInfoMsg)
{
	commonStereoCallback(
			nav_msgs::OdometryConstPtr(),
			leftImageMsg,
			rightImageMsg,
			leftCameraInfoMsg,
			rightCameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			odomInfoMsg);
}

void GuiWrapper::stereoTFCallback(
		const sensor_msgs::ImageConstPtr& leftImageMsg,
		const sensor_msgs::ImageConstPtr& rightImageMsg,
		const sensor_msgs::CameraInfoConstPtr& leftCameraInfoMsg,
		const sensor_msgs::CameraInfoConstPtr& rightCameraInfoMsg)
{
	commonStereoCallback(
			nav_msgs::OdometryConstPtr(),
			leftImageMsg,
			rightImageMsg,
			leftCameraInfoMsg,
			rightCameraInfoMsg,
			sensor_msgs::LaserScanConstPtr(),
			rtabmap_ros::OdomInfoConstPtr());
}

void GuiWrapper::setupCallbacks(
		bool subscribeDepth,
		bool subscribeLaserScan,
		bool subscribeOdomInfo,
		bool subscribeStereo,
		int queueSize,
		int depthCameras)
{
	ros::NodeHandle nh; // public
	ros::NodeHandle pnh("~"); // private

	if(subscribeDepth && subscribeStereo)
	{
		ROS_WARN("\"subscribe_depth\" already true, ignoring \"subscribe_stereo\".");
	}
	if(!subscribeDepth && !subscribeStereo && subscribeLaserScan)
	{
		ROS_WARN("Cannot subscribe to laser scan without depth or stereo subscription...");
	}
	if(depthCameras <= 0)
	{
		depthCameras = 1;
	}
	if(depthCameras > 2)
	{
		ROS_WARN("Cannot subscribe to more than 2 cameras yet...");
		depthCameras = 2;
	}

	if(subscribeDepth)
	{
		UASSERT(depthCameras >= 1 && depthCameras <= 2);
		UASSERT_MSG(depthCameras == 1 || !(subscribeLaserScan || !odomFrameId_.empty()), "Not yet supported!");

		imageSubs_.resize(depthCameras);
		imageDepthSubs_.resize(depthCameras);
		cameraInfoSubs_.resize(depthCameras);
		for(int i=0; i<depthCameras; ++i)
		{
			std::string rgbPrefix = "rgb";
			std::string depthPrefix = "depth";
			if(depthCameras>1)
			{
				rgbPrefix += uNumber2Str(i);
				depthPrefix += uNumber2Str(i);
			}
			ros::NodeHandle rgb_nh(nh, rgbPrefix);
			ros::NodeHandle depth_nh(nh, depthPrefix);
			ros::NodeHandle rgb_pnh(pnh, rgbPrefix);
			ros::NodeHandle depth_pnh(pnh, depthPrefix);
			image_transport::ImageTransport rgb_it(rgb_nh);
			image_transport::ImageTransport depth_it(depth_nh);
			image_transport::TransportHints hintsRgb("raw", ros::TransportHints(), rgb_pnh);
			image_transport::TransportHints hintsDepth("raw", ros::TransportHints(), depth_pnh);

			imageSubs_[i] = new image_transport::SubscriberFilter;
			imageDepthSubs_[i] = new image_transport::SubscriberFilter;
			cameraInfoSubs_[i] = new message_filters::Subscriber<sensor_msgs::CameraInfo>;
			imageSubs_[i]->subscribe(rgb_it, rgb_nh.resolveName("image"), 1, hintsRgb);
			imageDepthSubs_[i]->subscribe(depth_it, depth_nh.resolveName("image"), 1, hintsDepth);
			cameraInfoSubs_[i]->subscribe(rgb_nh, "camera_info", 1);
		}

		if(odomFrameId_.empty())
		{
			odomSub_.subscribe(nh, "odom", 1);
			if(subscribeLaserScan)
			{
				scanSub_.subscribe(nh, "scan", 1);
				depthScanSync_ = new message_filters::Synchronizer<MyDepthScanSyncPolicy>(
						MyDepthScanSyncPolicy(queueSize),
						scanSub_,
						odomSub_,
						*imageSubs_[0],
						*imageDepthSubs_[0],
						*cameraInfoSubs_[0]);
				depthScanSync_->registerCallback(boost::bind(&GuiWrapper::depthScanCallback, this, _1, _2, _3, _4, _5));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageSubs_[0]->getTopic().c_str(),
						imageDepthSubs_[0]->getTopic().c_str(),
						cameraInfoSubs_[0]->getTopic().c_str(),
						odomSub_.getTopic().c_str(),
						scanSub_.getTopic().c_str());
			}
			else if(subscribeOdomInfo)
			{
				odomInfoSub_.subscribe(nh, "odom_info", 1);
				if(depthCameras > 1)
				{
					depthOdomInfo2Sync_ = new message_filters::Synchronizer<MyDepthOdomInfo2SyncPolicy>(
							MyDepthOdomInfo2SyncPolicy(queueSize),
							odomInfoSub_,
							odomSub_,
							*imageSubs_[0],
							*imageDepthSubs_[0],
							*cameraInfoSubs_[0],
							*imageSubs_[1],
							*imageDepthSubs_[1],
							*cameraInfoSubs_[1]);
					depthOdomInfo2Sync_->registerCallback(boost::bind(&GuiWrapper::depthOdomInfo2Callback, this, _1, _2, _3, _4, _5, _6, _7, _8));

					ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s\n   %s,\n   %s,\n   %s",
							ros::this_node::getName().c_str(),
							imageSubs_[0]->getTopic().c_str(),
							imageDepthSubs_[0]->getTopic().c_str(),
							cameraInfoSubs_[0]->getTopic().c_str(),
							imageSubs_[1]->getTopic().c_str(),
							imageDepthSubs_[1]->getTopic().c_str(),
							cameraInfoSubs_[1]->getTopic().c_str(),
							odomSub_.getTopic().c_str(),
							odomInfoSub_.getTopic().c_str());
				}
				else
				{
					depthOdomInfoSync_ = new message_filters::Synchronizer<MyDepthOdomInfoSyncPolicy>(
							MyDepthOdomInfoSyncPolicy(queueSize),
							odomInfoSub_,
							odomSub_,
							*imageSubs_[0],
							*imageDepthSubs_[0],
							*cameraInfoSubs_[0]);
					depthOdomInfoSync_->registerCallback(boost::bind(&GuiWrapper::depthOdomInfoCallback, this, _1, _2, _3, _4, _5));

					ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
							ros::this_node::getName().c_str(),
							imageSubs_[0]->getTopic().c_str(),
							imageDepthSubs_[0]->getTopic().c_str(),
							cameraInfoSubs_[0]->getTopic().c_str(),
							odomSub_.getTopic().c_str(),
							odomInfoSub_.getTopic().c_str());
				}
			}
			else
			{
				if(depthCameras > 1)
				{
					depth2Sync_ = new message_filters::Synchronizer<MyDepth2SyncPolicy>(
							MyDepth2SyncPolicy(queueSize),
							odomSub_,
							*imageSubs_[0],
							*imageDepthSubs_[0],
							*cameraInfoSubs_[0],
							*imageSubs_[1],
							*imageDepthSubs_[1],
							*cameraInfoSubs_[1]);
					depth2Sync_->registerCallback(boost::bind(&GuiWrapper::depth2Callback, this, _1, _2, _3, _4, _5, _6, _7));

					ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s\n   %s,\n   %s,\n   %s",
							ros::this_node::getName().c_str(),
							imageSubs_[0]->getTopic().c_str(),
							imageDepthSubs_[0]->getTopic().c_str(),
							cameraInfoSubs_[0]->getTopic().c_str(),
							imageSubs_[1]->getTopic().c_str(),
							imageDepthSubs_[1]->getTopic().c_str(),
							cameraInfoSubs_[1]->getTopic().c_str(),
							odomSub_.getTopic().c_str());
				}
				else
				{
					depthSync_ = new message_filters::Synchronizer<MyDepthSyncPolicy>(
							MyDepthSyncPolicy(queueSize),
							odomSub_,
							*imageSubs_[0],
							*imageDepthSubs_[0],
							*cameraInfoSubs_[0]);
					depthSync_->registerCallback(boost::bind(&GuiWrapper::depthCallback, this, _1, _2, _3, _4));

					ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s",
							ros::this_node::getName().c_str(),
							imageSubs_[0]->getTopic().c_str(),
							imageDepthSubs_[0]->getTopic().c_str(),
							cameraInfoSubs_[0]->getTopic().c_str(),
							odomSub_.getTopic().c_str());
				}
			}
		}
		else
		{
			// use TF as odom
			if(subscribeLaserScan)
			{
				scanSub_.subscribe(nh, "scan", 1);
				depthScanTFSync_ = new message_filters::Synchronizer<MyDepthScanTFSyncPolicy>(
						MyDepthScanTFSyncPolicy(queueSize),
						scanSub_,
						*imageSubs_[0],
						*imageDepthSubs_[0],
						*cameraInfoSubs_[0]);
				depthScanTFSync_->registerCallback(boost::bind(&GuiWrapper::depthScanTFCallback, this, _1, _2, _3, _4));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageSubs_[0]->getTopic().c_str(),
						imageDepthSubs_[0]->getTopic().c_str(),
						cameraInfoSubs_[0]->getTopic().c_str(),
						scanSub_.getTopic().c_str());
			}
			else if(subscribeOdomInfo)
			{
				odomInfoSub_.subscribe(nh, "odom_info", 1);
				depthOdomInfoTFSync_ = new message_filters::Synchronizer<MyDepthOdomInfoTFSyncPolicy>(
						MyDepthOdomInfoTFSyncPolicy(queueSize),
						odomInfoSub_,
						*imageSubs_[0],
						*imageDepthSubs_[0],
						*cameraInfoSubs_[0]);
				depthOdomInfoTFSync_->registerCallback(boost::bind(&GuiWrapper::depthOdomInfoTFCallback, this, _1, _2, _3, _4));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageSubs_[0]->getTopic().c_str(),
						imageDepthSubs_[0]->getTopic().c_str(),
						cameraInfoSubs_[0]->getTopic().c_str(),
						odomInfoSub_.getTopic().c_str());
			}
			else
			{
				depthTFSync_ = new message_filters::Synchronizer<MyDepthTFSyncPolicy>(
						MyDepthTFSyncPolicy(queueSize),
						*imageSubs_[0],
						*imageDepthSubs_[0],
						*cameraInfoSubs_[0]);
				depthTFSync_->registerCallback(boost::bind(&GuiWrapper::depthTFCallback, this, _1, _2, _3));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageSubs_[0]->getTopic().c_str(),
						imageDepthSubs_[0]->getTopic().c_str(),
						cameraInfoSubs_[0]->getTopic().c_str());
			}
		}
	}
	else if(subscribeStereo)
	{
		ros::NodeHandle left_nh(nh, "left");
		ros::NodeHandle right_nh(nh, "right");
		ros::NodeHandle left_pnh(pnh, "left");
		ros::NodeHandle right_pnh(pnh, "right");
		image_transport::ImageTransport left_it(left_nh);
		image_transport::ImageTransport right_it(right_nh);
		image_transport::TransportHints hintsLeft("raw", ros::TransportHints(), left_pnh);
		image_transport::TransportHints hintsRight("raw", ros::TransportHints(), right_pnh);

		imageRectLeft_.subscribe(left_it, left_nh.resolveName("image_rect"), 1, hintsLeft);
		imageRectRight_.subscribe(right_it, right_nh.resolveName("image_rect"), 1, hintsRight);
		cameraInfoLeft_.subscribe(left_nh, "camera_info", 1);
		cameraInfoRight_.subscribe(right_nh, "camera_info", 1);

		if(odomFrameId_.empty())
		{
			odomSub_.subscribe(nh, "odom", 1);
			if(subscribeLaserScan)
			{
				scanSub_.subscribe(nh, "scan", 1);
				stereoScanSync_ = new message_filters::Synchronizer<MyStereoScanSyncPolicy>(
						MyStereoScanSyncPolicy(queueSize),
						scanSub_,
						odomSub_,
						imageRectLeft_,
						imageRectRight_,
						cameraInfoLeft_,
						cameraInfoRight_);
				stereoScanSync_->registerCallback(boost::bind(&GuiWrapper::stereoScanCallback, this, _1, _2, _3, _4, _5, _6));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageRectLeft_.getTopic().c_str(),
						imageRectRight_.getTopic().c_str(),
						cameraInfoLeft_.getTopic().c_str(),
						cameraInfoRight_.getTopic().c_str(),
						odomSub_.getTopic().c_str(),
						scanSub_.getTopic().c_str());
			}
			else if(subscribeOdomInfo)
			{
				odomInfoSub_.subscribe(nh, "odom_info", 1);
				stereoOdomInfoSync_ = new message_filters::Synchronizer<MyStereoOdomInfoSyncPolicy>(
						MyStereoOdomInfoSyncPolicy(queueSize),
						odomInfoSub_,
						odomSub_,
						imageRectLeft_,
						imageRectRight_,
						cameraInfoLeft_,
						cameraInfoRight_);
				stereoOdomInfoSync_->registerCallback(boost::bind(&GuiWrapper::stereoOdomInfoCallback, this, _1, _2, _3, _4, _5, _6));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageRectLeft_.getTopic().c_str(),
						imageRectRight_.getTopic().c_str(),
						cameraInfoLeft_.getTopic().c_str(),
						cameraInfoRight_.getTopic().c_str(),
						odomSub_.getTopic().c_str(),
						odomInfoSub_.getTopic().c_str());
			}
			else
			{
				stereoSync_ = new message_filters::Synchronizer<MyStereoSyncPolicy>(
						MyStereoSyncPolicy(queueSize),
						odomSub_,
						imageRectLeft_,
						imageRectRight_,
						cameraInfoLeft_,
						cameraInfoRight_);
				stereoSync_->registerCallback(boost::bind(&GuiWrapper::stereoCallback, this, _1, _2, _3, _4, _5));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageRectLeft_.getTopic().c_str(),
						imageRectRight_.getTopic().c_str(),
						cameraInfoLeft_.getTopic().c_str(),
						cameraInfoRight_.getTopic().c_str(),
						odomSub_.getTopic().c_str());
			}
		}
		else
		{
			//use odom TF
			if(subscribeLaserScan)
			{
				scanSub_.subscribe(nh, "scan", 1);
				stereoScanTFSync_ = new message_filters::Synchronizer<MyStereoScanTFSyncPolicy>(
						MyStereoScanTFSyncPolicy(queueSize),
						scanSub_,
						imageRectLeft_,
						imageRectRight_,
						cameraInfoLeft_,
						cameraInfoRight_);
				stereoScanTFSync_->registerCallback(boost::bind(&GuiWrapper::stereoScanTFCallback, this, _1, _2, _3, _4, _5));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageRectLeft_.getTopic().c_str(),
						imageRectRight_.getTopic().c_str(),
						cameraInfoLeft_.getTopic().c_str(),
						cameraInfoRight_.getTopic().c_str(),
						scanSub_.getTopic().c_str());
			}
			else if(subscribeOdomInfo)
			{
				odomInfoSub_.subscribe(nh, "odom_info", 1);
				stereoOdomInfoTFSync_ = new message_filters::Synchronizer<MyStereoOdomInfoTFSyncPolicy>(
						MyStereoOdomInfoTFSyncPolicy(queueSize),
						odomInfoSub_,
						imageRectLeft_,
						imageRectRight_,
						cameraInfoLeft_,
						cameraInfoRight_);
				stereoOdomInfoTFSync_->registerCallback(boost::bind(&GuiWrapper::stereoOdomInfoTFCallback, this, _1, _2, _3, _4, _5));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageRectLeft_.getTopic().c_str(),
						imageRectRight_.getTopic().c_str(),
						cameraInfoLeft_.getTopic().c_str(),
						cameraInfoRight_.getTopic().c_str(),
						odomInfoSub_.getTopic().c_str());
			}
			else
			{
				stereoTFSync_ = new message_filters::Synchronizer<MyStereoTFSyncPolicy>(
						MyStereoTFSyncPolicy(queueSize),
						imageRectLeft_,
						imageRectRight_,
						cameraInfoLeft_,
						cameraInfoRight_);
				stereoTFSync_->registerCallback(boost::bind(&GuiWrapper::stereoTFCallback, this, _1, _2, _3, _4));

				ROS_INFO("\n%s subscribed to:\n   %s,\n   %s,\n   %s,\n   %s",
						ros::this_node::getName().c_str(),
						imageRectLeft_.getTopic().c_str(),
						imageRectRight_.getTopic().c_str(),
						cameraInfoLeft_.getTopic().c_str(),
						cameraInfoRight_.getTopic().c_str());
			}
		}
	}
	else // default odom only
	{
		defaultSub_ = nh.subscribe("odom", 1, &GuiWrapper::defaultCallback, this);

		ROS_INFO("\n%s subscribed to:\n   %s",
				ros::this_node::getName().c_str(),
				odomSub_.getTopic().c_str());
	}
}

