rtabmap_ros
===========

RTAB-Map's ROS package.

For more information, demos and tutorials about this package, visit the [rtabmap_ros](http://wiki.ros.org/rtabmap_ros) page on the ROS wiki.

For the RTAB-Map libraries and standalone application, visit the [RTAB-Map's home page](http://introlab.github.io/rtabmap) or the [RTAB-Map's wiki](https://github.com/introlab/rtabmap/wiki).

## Installation 

### ROS distribution 
RTAB-Map is released as binaries in the ROS distribution.
 * Jade
  ```
$ sudo apt-get install ros-jade-rtabmap-ros
```
 * Indigo
  ```
$ sudo apt-get install ros-indigo-rtabmap-ros
```
 * Hydro
  ```
$ sudo apt-get install ros-hydro-rtabmap-ros
```

### Build from source
This section shows how to install RTAB-Map ros-pkg on **ROS Hydro/Indigo/Jade** (Catkin build). RTAB-Map works only with the PCL 1.7, which is the default version installed with ROS Hydro/Indigo/Jade (**Fuerte and Groovy are not supported**).
 * **Note for ROS Indigo/Jade**: If you want SURF/SIFT, you have to build OpenCV from source to have access to *nonfree* module. Install it in `/usr/local` (default) and the rtabmap library should link with it instead of the one installed in ROS.

 * The next instructions assume that you have setup your ROS workspace using this [tutorial](http://wiki.ros.org/catkin/Tutorials/create_a_workspace). The workspace path is `~/catkin_ws` and your `~/.bashrc` contains:
 
  ```bash
source /opt/ros/[hydro|indigo|jade]/setup.bash
source ~/catkin_ws/devel/setup.bash
```

 1. First, you need to install the RTAB-Map standalone libraries (**don't checkout in the Catkin workspace** but install in your Catkin's devel folder).
 
 ```bash
$ cd ~
$ git clone https://github.com/introlab/rtabmap.git rtabmap
$ cd rtabmap/build
$ cmake -DCMAKE_INSTALL_PREFIX=~/catkin_ws/devel ..  [<---double dots included]
$ make -j4
$ make install
```

 2. Now install the RTAB-Map ros-pkg in your src folder of your Catkin workspace.
 
 ```bash
$ cd ~/catkin_ws
$ git clone https://github.com/introlab/rtabmap_ros.git src/rtabmap_ros
$ catkin_make
```

#### Update to new version 

```bash
$ cd rtabmap
$ git pull origin master
$ cd build
$ make
$ make install

$ roscd rtabmap_ros
$ git pull origin master
$ cd ~/catkin_ws
$ catkin_make
```


