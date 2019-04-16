#include <iostream>
#include <fstream>
#include <chrono>
#include <stdio.h>
#include <vector>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <opencv2/opencv.hpp>
#include <stdio.h>

#include "estimator.h"
#include "parameters.h"
#include "camodocal/camera_models/CameraFactory.h"
#include "camodocal/camera_models/CataCamera.h"
#include "camodocal/camera_models/PinholeCamera.h"
#include "feature_tracker.h"

#include "msgg/Imu.h"
#include "msgg/PointCloud.h"
#include "msgg/Odometry.h"

#include <pangolin/pangolin.h>

using namespace std;

vector<uchar> r_status;
vector<float> r_err;
FeatureTracker trackerData[NUM_OF_CAM];
double first_image_time;
int pub_count = 1;
bool first_image_flag = true;
bool init_pub = 0;
bool view_done = false;

Estimator estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
std::mutex m_posegraph_buf;
queue<int> optimize_posegraph_buf;

int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_loop_drift;
std::mutex m_keyframedatabase_resample;
std::mutex m_update_visualization;
std::mutex m_keyframe_buf;
std::mutex m_retrive_data_buf;

double latest_time;
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;

queue<pair<cv::Mat, double>> image_buf;

int global_frame_cnt = 0;
//camera param
camodocal::CameraPtr m_camera;
vector<int> erase_index;
std_msgs::Header cur_header;
Eigen::Vector3d relocalize_t{Eigen::Vector3d(0, 0, 0)};
Eigen::Matrix3d relocalize_r{Eigen::Matrix3d::Identity()};
bool init_imu=1;
bool init_feature = 0;


void pubOdometry(const Estimator &estimator, const std_msgs::Header &header)
{
    if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
    {

        // write result to file
        ofstream foutC(VINS_RESULT_PATH, ios::app);
        foutC.setf(ios::fixed, ios::floatfield);
        foutC.precision(0);
        foutC << header.stamp.toSec() * 1e9 << ",";
        foutC.precision(5);
        foutC << estimator.Ps[WINDOW_SIZE].x() << ","
              << estimator.Ps[WINDOW_SIZE].y() << ","
              << estimator.Ps[WINDOW_SIZE].z() << ","
              << tmp_Q.w() << ","
              << tmp_Q.x() << ","
              << tmp_Q.y() << ","
              << tmp_Q.z() << ","
              << estimator.Vs[WINDOW_SIZE].x() << ","
              << estimator.Vs[WINDOW_SIZE].y() << ","
              << estimator.Vs[WINDOW_SIZE].z() << "," << endl;
        foutC.close();
    }
}

void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();

    if (init_imu)
    {
        latest_time = t;
        init_imu = 0;
        return;
    }

    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};


    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;

//    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba - tmp_Q.inverse() * estimator.g);
//
//    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
//    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);
//
//    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba - tmp_Q.inverse() * estimator.g);
//
//    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);
//
//    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
//    tmp_V = tmp_V + dt * un_acc;
//
//    acc_0 = linear_acceleration;
//    gyr_0 = angular_velocity;
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = estimator.Ps[WINDOW_SIZE];
    tmp_Q = estimator.Rs[WINDOW_SIZE];
//    tmp_P = relocalize_r * estimator.Ps[WINDOW_SIZE] + relocalize_t;
//    tmp_Q = relocalize_r * estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}
vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>>  getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    while (true)
    {

        if (imu_buf.empty() || feature_buf.empty())
            return measurements;
        if (!(imu_buf.back()->header.stamp.toSec() > feature_buf.front()->header.stamp.toSec()+estimator.td))
        {
            //     ROS_WARN("wait for imu, only should happen at the beginning");
            cout << "***************WARN: wait for imu, only should happen at the beginning" << endl;
            sum_of_wait++;
            return measurements;
        }

        if (!(imu_buf.front()->header.stamp.toSec() < feature_buf.front()->header.stamp.toSec()+ estimator.td))
        {
            //    ROS_WARN("throw img, only should happen at the beginning");
            cout << "*************WARN: throw img, only should happen at the beginning" << endl;
            feature_buf.pop();
            continue;
        }
        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();
        feature_buf.pop();

        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec()+ estimator.td)
        {
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }
        IMUs.emplace_back(imu_buf.front());
// ROS_INFO_STREAM("IMUs end data timestamp: " << IMUs.back()->header.stamp << "IMUs size: "<< IMUs.size() << "img_msg timestamp" << img_msg->header.stamp );
        measurements.emplace_back(IMUs, img_msg);
    }
    return measurements;
}

void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
//    m_buf.lock();
    imu_buf.push(imu_msg);
//    m_buf.unlock();
//    con.notify_one();
    {
//        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);
    }

}

void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
//    m_buf.lock();
    if (!init_feature)
    {
        //skip the first detected feature, which doesn't contain optical flow speed
        //跳过第一个检测到的帧，该功能不包含光流速度
        init_feature = 1;
        return;
    }
    feature_buf.push(feature_msg);
    //ROS_INFO("----------feature timestamp %f------------",feature_msg->header.stamp.toSec());
//    m_buf.unlock();
//    con.notify_one();
}

void send_imu(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (current_time < 0)
        current_time = t;
    double dt = t - current_time;
    current_time = t;

    double ba[]{0.0, 0.0, 0.0};
    double bg[]{0.0, 0.0, 0.0};

    double dx = imu_msg->linear_acceleration.x - ba[0];
    double dy = imu_msg->linear_acceleration.y - ba[1];
    double dz = imu_msg->linear_acceleration.z - ba[2];

    double rx = imu_msg->angular_velocity.x - bg[0];
    double ry = imu_msg->angular_velocity.y - bg[1];
    double rz = imu_msg->angular_velocity.z - bg[2];
    //ROS_DEBUG("IMU %f, dt: %f, acc: %f %f %f, gyr: %f %f %f", t, dt, dx, dy, dz, rx, ry, rz);

    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
}

std::vector<Eigen::Vector3f>  mPoints;


// thread: visual-inertial odometry
void process()
{
//    while (true)
//    {
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
//        std::unique_lock<std::mutex> lk(m_buf);
//        con.wait(lk, [&]
//        {
//            return (measurements = getMeasurements()).size() != 0;
//        });
////        cout<<"************************************************************"<<endl;
//        lk.unlock();

        measurements = getMeasurements();

        for (auto &measurement : measurements)
        {
//            printf("imgtime: %10.6f, imu: %ld, firsttime: %10.6f,lasttime : %10.6f\n",measurement.second->header.stamp.toSec() ,measurement.first.size(),
//                      measurement.first[0]->header.stamp.toSec(),measurement.first.back()->header.stamp.toSec());
            auto img_msg = measurement.second;
            double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;
            for (auto &imu_msg : measurement.first)
            {
                double t = imu_msg->header.stamp.toSec();
                double img_t =img_msg->header.stamp.toSec() + estimator.td;
                if (t <= img_t)//判断imu观测时间是否比图像小，决定是否内插比力和角速度
                {
                    if (current_time < 0)
                        current_time = t;
                    double dt = t - current_time;
                    assert(dt >= 0);
                    current_time = t;
                    dx = imu_msg->linear_acceleration.x;
                    dy = imu_msg->linear_acceleration.y;
                    dz = imu_msg->linear_acceleration.z;
                    rx = imu_msg->angular_velocity.x;
                    ry = imu_msg->angular_velocity.y;
                    rz = imu_msg->angular_velocity.z;
                    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);

                }
                else
                {
                    double dt_1 = img_t - current_time;
                    double dt_2 = t - img_t;
                    current_time = img_t;
                    assert(dt_1 >= 0);
                    assert(dt_2 >= 0);
                    assert(dt_1 + dt_2 > 0);
                    double w1 = dt_2 / (dt_1 + dt_2);
                    double w2 = dt_1 / (dt_1 + dt_2);
                    dx = w1 * dx + w2 * imu_msg->linear_acceleration.x;
                    dy = w1 * dy + w2 * imu_msg->linear_acceleration.y;
                    dz = w1 * dz + w2 * imu_msg->linear_acceleration.z;
                    rx = w1 * rx + w2 * imu_msg->angular_velocity.x;
                    ry = w1 * ry + w2 * imu_msg->angular_velocity.y;
                    rz = w1 * rz + w2 * imu_msg->angular_velocity.z;
                    estimator.processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
                }
            }
//                send_imu(imu_msg);


            printf("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());
            TicToc t_s;
            map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image;
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                double p_u = img_msg->channels[1].values[i];
                double p_v = img_msg->channels[2].values[i];
                double velocity_x = img_msg->channels[3].values[i];
                double velocity_y = img_msg->channels[4].values[i];
                assert(z == 1);
                Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
                xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
                image[feature_id].emplace_back(camera_id,  xyz_uv_velocity);
            }
            estimator.processImage(image, img_msg->header);

            cout << "position: " << estimator.Ps[WINDOW_SIZE].transpose() << endl;
            
            if( estimator.Ps[WINDOW_SIZE].x() < 1e-6 &&
                estimator.Ps[WINDOW_SIZE].y() < 1e-6 &&
                estimator.Ps[WINDOW_SIZE].z() < 1e-6)
                continue;
            
           
            
            
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";
            pubOdometry(estimator, header);
//            m_loop_drift.lock();
        }
//        m_buf.lock();
//        m_state.lock();
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
        {
            mPoints.emplace_back(Eigen::Vector3f(estimator.Ps[WINDOW_SIZE].x(),
                                                 estimator.Ps[WINDOW_SIZE].y(),
                                                 estimator.Ps[WINDOW_SIZE].z()));
            update();
        }
//        m_state.unlock();
//        m_buf.unlock();
//    }
}


void LoadImages(const string &strImagePath, const string &strTimesStampsPath,
                vector<string> &strImagesFileNames, vector<double> &timeStamps)
{
    ifstream fTimes;
    fTimes.open(strTimesStampsPath.c_str());
    timeStamps.reserve(5000); //reserve vector space
    strImagesFileNames.reserve(5000);
    while(!fTimes.eof())
    {
        string s;
        getline(fTimes,s);
        if(!s.empty())
        {
            stringstream ss;
            ss << s;
            if(s[0] == '#')
                continue;
            char filename[255] = {0};
            double t;
            sscanf(ss.str().c_str(), "%lf,%s",&t,filename);
            strImagesFileNames.push_back(strImagePath + "/" + string(filename));
            
            timeStamps.push_back(t/1e9);
        }
    }
}
void img_callback(const cv::Mat &show_img,const ros::Time &timestamp)
{
    if(first_image_flag)//是不是第一帧的判断
    {
        first_image_flag = false;
        first_image_time = timestamp.toSec();
//        last_image_time = timestamp.toSec();//上一个照片时间
        return;
    }
    // frequency control  图像特征点输出频率的控制(图像发送频率为10hz)
    if (round(1.0 * pub_count / (timestamp.toSec() - first_image_time)) <= FREQ)
    {
        PUB_THIS_FRAME = true;
        // reset the frequency control
        if (abs(1.0 * pub_count / (timestamp.toSec() - first_image_time) - FREQ) < 0.01 * FREQ)
        {
            first_image_time = timestamp.toSec();
            pub_count = 0;
        }
    }
    else
        PUB_THIS_FRAME = false;

    //改变图像编码
    TicToc t_r;
    for (int i = 0; i < NUM_OF_CAM; i++)
    {
//        cout<<"processing camera "<<i<<endl;
        if (i != 1 || !STEREO_TRACK)
            trackerData[i].readImage(show_img.rowRange(ROW * i, ROW * (i + 1)), timestamp.toSec());
        else
        {
            if (EQUALIZE)
            {
                cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
                clahe->apply(show_img.rowRange(ROW * i, ROW * (i + 1)), trackerData[i].cur_img);
            }
            else
                trackerData[i].cur_img = show_img.rowRange(ROW * i, ROW * (i + 1));
        }

    }

    for (unsigned int i = 0;; i++)
    {
        bool completed = false;
        for (int j = 0; j < NUM_OF_CAM; j++)
            if (j != 1 || !STEREO_TRACK)
                completed |= trackerData[j].updateID(i);//更新后检测点的id信息
        if (!completed)
            break;
    }

    if (PUB_THIS_FRAME)
    {
        pub_count++;
        sensor_msgs::PointCloudPtr feature_points(new sensor_msgs::PointCloud);
        sensor_msgs::ChannelFloat32 id_of_point;
        sensor_msgs::ChannelFloat32 u_of_point;
        sensor_msgs::ChannelFloat32 v_of_point;
        sensor_msgs::ChannelFloat32 velocity_x_of_point;
        sensor_msgs::ChannelFloat32 velocity_y_of_point;

        feature_points->header.stamp= timestamp;
        feature_points->header.frame_id = "world";

        vector<set<int>> hash_ids(NUM_OF_CAM);
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            auto &un_pts = trackerData[i].cur_un_pts;
            auto &cur_pts = trackerData[i].cur_pts;
            auto &ids = trackerData[i].ids;
            auto &pts_velocity = trackerData[i].pts_velocity;
            for (unsigned int j = 0; j < ids.size(); j++)
            {
                if (trackerData[i].track_cnt[j] > 1)
                {
                    int p_id = ids[j];
                    hash_ids[i].insert(p_id);
                    geometry_msgs::Point32 p;
                    p.x = un_pts[j].x;
                    p.y = un_pts[j].y;
                    p.z = 1;

                    feature_points->points.push_back(p);
                    id_of_point.values.push_back(p_id * NUM_OF_CAM + i);
                    u_of_point.values.push_back(cur_pts[j].x);
                    v_of_point.values.push_back(cur_pts[j].y);
                    velocity_x_of_point.values.push_back(pts_velocity[j].x);
                    velocity_y_of_point.values.push_back(pts_velocity[j].y);
                }
            }
        }
        feature_points->channels.push_back(id_of_point);
        feature_points->channels.push_back(u_of_point);
        feature_points->channels.push_back(v_of_point);
        feature_points->channels.push_back(velocity_x_of_point);
        feature_points->channels.push_back(velocity_y_of_point);
        if (!init_pub)
        {
            init_pub = 1;
        }
        else
            feature_callback(feature_points);

        if (SHOW_TRACK)
        {
            cv::Mat tmp_img = show_img.rowRange(0, ROW);
            cv::cvtColor(show_img, tmp_img, CV_GRAY2RGB);
            for (unsigned int j = 0; j < trackerData[0].cur_pts.size(); j++)
            {
                double len = std::min(1.0, 1.0 * trackerData[0].track_cnt[j] / 20);
                cv::circle(tmp_img, trackerData[0].cur_pts[j], 2, cv::Scalar(255 * (1 - len), 0, 255 * len), 2);
            }
//            cv::namedWindow("vins", CV_WINDOW_NORMAL);
//            cv::imshow("vins", tmp_img);
//            cv::waitKey(1);
        }
    }
}

void LoadImus(ifstream & fImus, const ros::Time &imageTimestamp)
{

    while(!fImus.eof())
    {
        string s;
        getline(fImus,s);
        if(!s.empty())
        {
            char c = s.at(0);
            if(c<'0' || c>'9')      //remove first line in data.csv
                continue;
            stringstream ss;
            ss << s;
            double tmpd;
            int cnt=0;
            double data[7];
            while(ss >> tmpd)
            {
                data[cnt] = tmpd;
                cnt++;
                if(cnt ==7)
                    break;
                if(ss.peek() == ',' || ss.peek() == ' ')
                    ss.ignore();
            }
            data[0] *=1e-9; //convert to second unit
            sensor_msgs::ImuPtr imudata(new sensor_msgs::Imu);
            imudata->angular_velocity.x = data[1];
            imudata->angular_velocity.y = data[2];
            imudata->angular_velocity.z = data[3];
            imudata->linear_acceleration.x = data[4];
            imudata->linear_acceleration.y = data[5];
            imudata->linear_acceleration.z = data[6];
            uint32_t  sec = data[0];
            uint32_t nsec = (data[0]-sec)*1e9;
            nsec = (nsec/1000)*1000+500;
            imudata->header.stamp = ros::Time(sec,nsec);
            imu_callback(imudata);
//            cout<<data[0]<<endl;
            if (imudata->header.stamp > imageTimestamp)       //load all imu data produced in interval time between two consecutive frams
                break;
        }
    }
}
void LoadGPS( double time)
{
    ifstream fGPS(GPS_file);
    while(!fGPS.eof())
    {
        string s;
        getline(fGPS,s);
        if(!s.empty())
        {
            stringstream ss;
            ss << s;
            double tmpd;
            int cnt=0;
            double data[4];
            while(ss >> tmpd)
            {
                data[cnt] = tmpd;
                cnt++;
                if(cnt ==4)
                    break;
                if(ss.peek() == ',' || ss.peek() == ' ')
                    ss.ignore();
            }
            data[0] *=1e-9;
            if(data[0]<time)
                continue;
            else
            {
                gps_struct gpsdata;
                gpsdata.time=data[0];
                gpsdata.gpspos[0]=data[1];
                gpsdata.gpspos[1]=data[2];
                gpsdata.gpspos[2]=data[3];
                gpsdata.gpscov[0]=0.01*0.01;
                gpsdata.gpscov[4]=0.01*0.01;
                gpsdata.gpscov[8]=0.01*0.01;
                estimator.gpsvec.push_back(gpsdata);
            }
        }
    }
    fGPS.close();

}
pangolin::View *mpView = NULL;
 pangolin::OpenGlRenderState mCam;
int mWidth(1024);
int mHeight(768);
void display()
{
    static bool first = true;
    if(first)
    {
        pangolin::OpenGlMatrix Twc;
        pangolin::CreateWindowAndBind("ORB-SLAM2: Map Viewer",mWidth,mHeight);
        
        // 3D Mouse handler requires depth testing to be enabled
        glEnable(GL_DEPTH_TEST);
        
        // Issue specific OpenGl we might need
        glEnable (GL_BLEND);
        glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        // Add named OpenGL viewport to window and provide 3D Handler
        
        Twc.SetIdentity();
        
        // wLeft : hTop = w : h
        mCam = pangolin::OpenGlRenderState(
                                           pangolin::ProjectionMatrix(mWidth,mHeight,mWidth,mWidth,mWidth >> 1,mHeight >> 1,0.001,100),
                                           pangolin::ModelViewLookAt(-2,-2,-2, 0,0,0, pangolin::AxisY)
                                           );
        const int border = 0;
        pangolin::CreatePanel("menu").SetBounds(0.0,1.0,0.0,pangolin::Attach::Pix(border));
        mpView = &pangolin::CreateDisplay()
        .SetBounds(0.0, 1.0, pangolin::Attach::Pix(border), 1.0, -mWidth/(float)mHeight)
        .SetHandler(new pangolin::Handler3D(mCam));
        first = false;
    }
    
    
    mpView->Activate(mCam);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glClearColor(1.0f,1.0f,1.0f,1.0f);
    
    pangolin::glDrawAxis(1);

    
   
    glPointSize(3.0);
    glBegin(GL_POINTS);
    for(int i = 0; i < mPoints.size(); ++i)
    {
        if(0 != i)
            glColor3f(1.0, 0.0, 0.0);
        else
            glColor3f(0.0,1.0,0.0);
        
        glVertex3f(mPoints[i].x(), mPoints[i].y(), mPoints[i].z());
    }
    glEnd();
    
    pangolin::FinishFrame();
    
}




int main(int argc, char **argv)
{
    /******************* load image begin ***********************/

    readParameters("/Volumes/mac/SlamResources/VINS_GPS_OSX/config/euroc/euroc_config.yaml");
    //imu data file
    ifstream fImus;
    // fImus.open("/home/grn/CLionProjects/VINS/data/MH_03_medium/mav0/imu0/imudata.csv");
    fImus.open(IMU_file);
    cv::Mat image;
    int ni;//num image

    //read parameters section
    

    estimator.setParameter();
    for (int i = 0; i < NUM_OF_CAM; i++)
        trackerData[i].readIntrinsicParameter(CAM_NAMES[i]); 

    vector<string> vStrImagesFileNames;
    vector<double> vTimeStamps;
    // LoadImages(string("/home/grn/CLionProjects/VINS/data/MH_03_medium/mav0/cam0/left"),
    //         string("/home/grn/CLionProjects/VINS/data/MH_03_medium/mav0/cam0/ldata.csv"),vStrImagesFileNames,vTimeStamps);
    LoadImages(IMAGE_data,IMAGE_file,vStrImagesFileNames,vTimeStamps);

    int imageNum = vStrImagesFileNames.size();

    if(imageNum<=0)
    {
        cerr << "ERROR: Failed to load images" << endl;
        return 1;
    }

    LoadGPS(vTimeStamps[0]);
    cout<<estimator.gpsvec.size()<<endl;




//    std::thread measurement_process{process};
//
//    measurement_process.detach();

    display();
    for(ni=0; ni<imageNum; ni++)
    {

        double  tframe = vTimeStamps[ni];   //timestamp
        if(abs(tframe-1403636581.063556)<0.00005)
            int nnn=1;
        uint32_t  sec = tframe;
        uint32_t nsec = (tframe-sec)*1e9;
        nsec = (nsec/1000)*1000+500;
        ros::Time image_timestamp = ros::Time(sec, nsec);
        // read imu data
        LoadImus(fImus,image_timestamp);
        //read image from file
        image = cv::imread(vStrImagesFileNames[ni],CV_LOAD_IMAGE_UNCHANGED);

        if(image.empty())
        {
            cerr << endl << "Failed to load image: " << vStrImagesFileNames[ni] <<endl;
            return 1;
        }
        std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
        img_callback(image, image_timestamp);
        std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
        double timeSpent =std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1).count();
        process();
        //wait to load the next frame image
        double T=0;
        if(ni < imageNum-1)
            T = vTimeStamps[ni+1]-tframe; //interval time between two consecutive frames,unit:second
        else if(ni>0)    //lastest frame
            T = tframe-vTimeStamps[ni-1];

       
        display();
        if(timeSpent < T)
            usleep((T-timeSpent)*1e6); //sec->us:1e6
        else
            cerr << endl << "process image speed too slow, larger than interval time between two consecutive frames" << endl;

    }
/******************* load image end ***********************/
    return 0;
}
