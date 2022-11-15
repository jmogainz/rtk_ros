/**
 * @file rtk_node.h
 * Node connecting to the RTK device and reading messages 
 * @author Alexis Paques <alexis.paques@gmail.com>
 */

#pragma once

#include <sstream>
#include <string>
#include <iostream>

#include <ros/ros.h>

#include <serial/serial.h>
#include <mavros_msgs/RTCM.h>
#include <sensor_msgs/NavSatFix.h>
#include <rtk_ros/GpsDrivers/src/ubx.h>
#include <rtk_ros/GpsDrivers/src/ashtech.h>
#include <rtk_ros/GpsDrivers/src/gps_helper.h>
#include "definitions.h"

class RTKNode
{
public:
	RTKNode(ros::NodeHandle * _nh,
        unsigned _baud = 0, 
        std::string _port = std::string("/dev/ttyACM0"),
        float _surveyAccuracy = 1.0,
        float _surveyDuration = 90.0):
        connected(false), baud(_baud), port(_port),
        surveyAccuracy(_surveyAccuracy), surveyDuration(_surveyDuration), nh(_nh) {
            surveyInStatus = new SurveyInStatus();
            pReportSatInfo = new satellite_info_s();
            RTCMPublisher = nh->advertise<mavros_msgs::RTCM>("/mavros/gps_rtk/send_rtcm", 1);
            GPSPublisher = nh->advertise<sensor_msgs::NavSatFix>("gps", 1);
    };
	~RTKNode() {
        if (gpsDriver) {
            delete gpsDriver;
            gpsDriver = nullptr;
        }
        if (serial) {
            delete serial;
            serial = nullptr;
        }
        if (pReportSatInfo) {
            delete pReportSatInfo;
            pReportSatInfo = nullptr;
        }
    };

    int status_px4_to_ros(int status) {
        if(status == 0) return -1;
        if(status == 1) return -1;
        if(status == 2) return -1;
        if(status == 3) return 0; // FIX
        if(status == 4) return 2; // Sat augmentation
        if(status == 5) return 2; //
        if(status == 6) return 2;
        if(status == 8) return 1;
        else return -1;
    }

    void connect() {
        if (!serial) serial = new serial::Serial();
        serial->setPort(port);

        serial->open();
        if (!serial->isOpen()) {
            ROS_WARN_STREAM("GPS: Failed to open Serial Device: " << port);
            return;
        }
        serial->setBaudrate(baud);
        serial->setBytesize(serial::eightbits);
        serial->setParity(serial::parity_none);
        serial->setStopbits(serial::stopbits_one);
        serial->setFlowcontrol(serial::flowcontrol_none);

        for (int tries = 0; tries < 5; tries++) {
            try {
                ROS_DEBUG("Trying to connect to the serial port");
                serial->open();
            } catch (serial::IOException) {
            } catch (...) {
                ROS_FATAL("Other serial port exception");
            }

            if (serial->isOpen()) {
                connected = true;
                serial::Timeout timeout = serial::Timeout::simpleTimeout(500);
                serial->setTimeout(timeout);
                return;
            } else {
                connected = false;
                ROS_INFO_STREAM("Bad Connection with serial port Error " << port);
            }
        }
    };

    void run() {
        if (gpsDriver->configure(baud, GPSDriverUBX::OutputMode::RTCM) == 0) {
            ROS_INFO("Configured");
            /* reset report */
            memset(&reportGPSPos, 0, sizeof(reportGPSPos));

            //In rare cases it can happen that we get an error from the driver (eg. checksum failure) due to
            //bus errors or buggy firmware. In this case we want to try multiple times before giving up.
            int numTries = 0;

            while (ros::ok() && numTries < 3) {
                int helperRet = gpsDriver->receive(100);
                ROS_DEBUG("Reading data");

                if (helperRet > 0) {
                    numTries = 0;

                    if (helperRet & 1) {
                        publishGPSPosition();
                        numTries = 0;
                    }

                    if (pReportSatInfo && (helperRet & 2)) {
                        publishGPSSatellite();
                        numTries = 0;
                    }
                } else {
                    ++numTries;
                }
            }

            // if (_serial->error() != Serial::NoError && _serial->error() != Serial::TimeoutError) {
            //     break;
            // }
        }

        ROS_WARN("End of running");
    };

    void publishGPSPosition() {
        // reportGPSPos
        sensor_msgs::NavSatFix msg;
        msg.header.stamp = ros::Time::now();
        msg.header.frame_id = "rtk_base";
        msg.altitude = reportGPSPos.alt;
        msg.longitude = reportGPSPos.lon;
        msg.latitude = reportGPSPos.lat;
        msg.status.status = status_px4_to_ros(reportGPSPos.fix_type);
        msg.status.service = msg.status.SERVICE_GPS;
        msg.position_covariance[0] = reportGPSPos.eph; // or hdop
        msg.position_covariance[4] = reportGPSPos.eph; // or hdop
        msg.position_covariance[8] = reportGPSPos.epv; // or vdop
        msg.position_covariance_type = sensor_msgs::NavSatFix::COVARIANCE_TYPE_APPROXIMATED;
        ROS_DEBUG_STREAM("** Publish pose **" 
            << std::endl << "altitude: " << reportGPSPos.alt
            << std::endl << "fix type: " << (int)reportGPSPos.fix_type
            << std::endl << "HDOP: "     << reportGPSPos.hdop << "\t VDOP" << reportGPSPos.vdop
            << std::endl << "lat: " << reportGPSPos.lat << "\t lon:" << reportGPSPos.lon
            << std::endl << "heading: " << reportGPSPos.heading
            << std::endl << "sat used: " << (int)reportGPSPos.satellites_used);
        GPSPublisher.publish(msg);
    };

    void publishGPSSatellite() {
        ROS_WARN_STREAM_THROTTLE(0.1, "I see " << (int)pReportSatInfo->count << " sattelites");
        // pReportSatInfo
    };

    void connect_gps() {
        // dynamic model
        uint8_t stationary_model = 2;
        ROS_INFO("Connect Driver");
        gpsDriver = new GPSDriverUBX(GPSDriverUBX::Interface::UART, &callbackEntry, this, &reportGPSPos, pReportSatInfo, stationary_model);
        gpsDriver->setSurveyInSpecs(surveyAccuracy * 10000, surveyDuration);
        ROS_INFO("Configure survey");
        memset(&reportGPSPos, 0, sizeof(reportGPSPos)); // Reset report
    };


    static int callbackEntry(GPSCallbackType type, void *data1, int data2, void *user)
    {
        RTKNode *node = (RTKNode *)user;
        return node->callback(type, data1, data2);
    };


    void gotRTCMData(uint8_t *data, size_t len) {
        mavros_msgs::RTCM msg;
        msg.data.resize(len);
        msg.data.assign(data, data + len);
        RTCMPublisher.publish(msg);
        ROS_WARN("Publish RTCM");
    }

    int callback(GPSCallbackType type, void *data1, int data2)
    {
        int bytes_written = 0;
        switch (type) {
            case GPSCallbackType::readDeviceData: {
                ROS_DEBUG("Read more data");
                
                if (serial->available() == 0) {
                    int timeout = *((int *) data1);
                    //if (!_serial->waitForReadyRead(timeout))
                    if (!serial->waitReadable())
                        return 0; // error, no new data
                }
                return (int)serial->read((uint8_t *) data1, data2);
            }
            case GPSCallbackType::writeDeviceData: {
                ROS_DEBUG("Write device data");
                bytes_written = serial->write((uint8_t *) data1, data2);
                if (bytes_written == data2) {
                    return data2;
                }
                return -1;
            }

            case GPSCallbackType::setBaudrate: {
                ROS_DEBUG("Set baudrate");
                serial->setBaudrate(data2);
                return true;
            }

            case GPSCallbackType::gotRTCMMessage: {
                ROS_FATAL("RTCM");
                gotRTCMData((uint8_t*) data1, data2);
                break;
            }

            case GPSCallbackType::surveyInStatus: {
                ROS_DEBUG("Survey");
                surveyInStatus = (SurveyInStatus*)data1;
                ROS_DEBUG_STREAM("Survey-in status: " << surveyInStatus->duration  << " cur accuracy: " << surveyInStatus->mean_accuracy 
                        << " valid:" << (int)(surveyInStatus->flags & 1) << " active: " << (int)((surveyInStatus->flags>>1) & 1));
                break;
            }

            case GPSCallbackType::setClock: {
                ROS_DEBUG("Set clock");
                break;
            }
            default: {
                ROS_FATAL_STREAM("Do nothing? " << (int)type);
                break;
            }
        }

        return 0;
    };


    bool connected;
private:
    ros::Publisher GPSPublisher;
    ros::Publisher RTCMPublisher;
    ros::NodeHandle * nh;
    unsigned baud;
    std::string port;
    float surveyAccuracy;
    float surveyDuration;
    SurveyInStatus* surveyInStatus = nullptr;
    GPSHelper* gpsDriver = nullptr;
    serial::Serial* serial = nullptr;
	struct vehicle_gps_position_s	reportGPSPos;
	struct satellite_info_s		*pReportSatInfo = nullptr;
};
