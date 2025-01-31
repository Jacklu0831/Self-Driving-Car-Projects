#include <uWS/uWS.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "helpers.h"
#include "json.hpp"
#include "spline.h"

// for convenience
using nlohmann::json;
using std::string;
using std::vector;

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  std::ifstream in_map_(map_file_.c_str(), std::ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    std::istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }

  // initialize lane number and reference velocity
  double ref_vel = 0.0;
  int lane = 1;
  
  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,
               &map_waypoints_dx,&map_waypoints_dy,&lane,&ref_vel]
              (uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
               uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
          // j[1] is the data JSON object
          
          // Main car's localization Data
          double car_x = j[1]["x"];
          double car_y = j[1]["y"];
          double car_s = j[1]["s"];
          double car_d = j[1]["d"];
          double car_yaw = j[1]["yaw"];
          double car_speed = j[1]["speed"];

          // Previous path data given to the Planner
          auto previous_path_x = j[1]["previous_path_x"];
          auto previous_path_y = j[1]["previous_path_y"];
          // Previous path's end s and d values 
          double end_path_s = j[1]["end_path_s"];
          double end_path_d = j[1]["end_path_d"];

          // Sensor Fusion Data, a list of all other cars on the same side 
          //   of the road.
          auto sensor_fusion = j[1]["sensor_fusion"];

          int prev_size = previous_path_x.size();
          if (prev_size > 0){
            car_s = end_path_s;
          }
          
          // initialize locations of traffic
          bool car_ahead = false;
          bool car_left = false;
          bool car_right = false;
          
          int sense_size = sensor_fusion.size();
          for (int i=0; i<sense_size; i++){
            double d = sensor_fusion[i][6];
            // initialize car lane number
            int car_lane = -1;
            // find car lane
            if (d>0 && d<4)
              car_lane = 0;
            else if (d>4 && d<8)
              car_lane = 1;
            else if (d>8 && d<12)
              car_lane = 2;
            else
              continue;
            
            // get other necessary info from sensors (det <-> detected)
            double velo_x = sensor_fusion[i][3];
            double velo_y = sensor_fusion[i][4];
            double det_car_s = sensor_fusion[i][5];
            double det_car_speed = sqrt(velo_x*velo_x + velo_y*velo_y);
            
            // predict and check car "s"
            det_car_s += (double)prev_size * 0.02 * det_car_speed;
            
            // car in our lane is front within 30
            if (car_lane == lane){
              if (det_car_s - car_s > 0 && det_car_s - car_s < 30)
                car_ahead = true;
            }
            // left in the range of -30 to 20
            else if (car_lane + 1 == lane){
              if (car_s < det_car_s + 20 && car_s > det_car_s -30)
                car_left = true;
            }
            // right in the range of -30 to 20
            else if (car_lane - 1 == lane){
              if (car_s < det_car_s + 20 && car_s > det_car_s -30)
                car_right = true;
            }
          }
          
          // center lane has both left and right choise, keep in it
          // do not make lane changes when not necessary
          // change lane to left is favored against right
          // brake if all else fails
          double speed_diff = 0;
          double kMaxSpeed = 49.5;
          double kMaxAcc = .224;
          if (car_ahead){
            if (car_left == false && lane > 0)
              lane--;
            else if (car_right == false && lane < 2)
              lane++;
            else
              speed_diff = -kMaxAcc;
          }
          else {
            if ((lane == 2 && car_left == false) || 
                (lane == 0 && car_right == false))
              lane = 1;
            if (ref_vel < kMaxSpeed)
              speed_diff = kMaxAcc;
          }
          
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          vector<double> ptsx;
          vector<double> ptsy;

          // if only one or less previous size, simply use the current state
          if (prev_size < 2){
            double prev_car_x = ref_x - cos(ref_yaw);
            double prev_car_y = ref_y - sin(ref_yaw);

            ptsx.push_back(prev_car_x);
            ptsx.push_back(car_x);
            ptsy.push_back(prev_car_y);
            ptsy.push_back(car_y);
          }
          else {
            // take the last state as the x reference
            ref_x = previous_path_x[prev_size - 1];
            ref_y = previous_path_y[prev_size - 1];
            double ref_x_prev = previous_path_x[prev_size - 2];
            double ref_y_prev = previous_path_y[prev_size - 2];

            ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

            ptsx.push_back(ref_x_prev);
            ptsx.push_back(ref_x);
            ptsy.push_back(ref_y_prev);
            ptsy.push_back(ref_y);
          }

          // Add 30 m spaced points in Frenet coordinates to the x y vectors
          vector<double> next_wp0 = getXY(car_s + 30, 
            (2 + 4 * lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp1 = getXY(car_s + 60, 
            (2 + 4 * lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp2 = getXY(car_s + 90, 
            (2 + 4 * lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);

          ptsx.push_back(next_wp0[0]);
          ptsx.push_back(next_wp1[0]);
          ptsx.push_back(next_wp2[0]);
          ptsy.push_back(next_wp0[1]);
          ptsy.push_back(next_wp1[1]);
          ptsy.push_back(next_wp2[1]);

          // shift angles to 0 degrees
          for (int i = 0; i < ptsx.size(); i++) {
              double del_x = ptsx[i] - ref_x;
              double del_y = ptsy[i] - ref_y;
              ptsx[i] = del_x * cos(-ref_yaw) - del_y * sin(-ref_yaw);
              ptsy[i] = del_x * sin(-ref_yaw) + del_y * cos(-ref_yaw);
          }

          // use spline.h library for interpolation
          tk::spline spl;
          spl.set_points(ptsx, ptsy);

          // initialize and fill the values to be communicated with the simulator
          vector<double> next_x_vals;
          vector<double> next_y_vals;

          for (int i = 0; i < previous_path_x.size(); ++i) {
            next_x_vals.push_back(previous_path_x[i]);
            next_y_vals.push_back(previous_path_y[i]);
          }

          // use spline to get target distance with delta x set as 30
          double target_x = 30.0;
          double target_y = spl(target_x);
          double target_dist = sqrt((target_x) * (target_x) + (target_y) * (target_y));

          // initialize x starting point for the for loop
          double x_start = 0;

          // predict next path points in total (constant speed diff)
          for (int i=0; i<= 50-previous_path_x.size(); i++){
            if (ref_vel + speed_diff > kMaxSpeed)
              ref_vel = kMaxSpeed;
            else
              ref_vel += speed_diff;

            // 0.02 second, 2.237 mph = 1 km/1000s
            double x = x_start + target_x / (target_dist / (0.02 * ref_vel / 2.237));
            double y = spl(x);

            // restart x_start for next coordinate
            x_start = x;

            // cache x and y
            double x_ref = x;
            double y_ref = y;

            // rotate coordinates to global
            x = (x_ref * cos(ref_yaw) - y_ref * sin(ref_yaw));
            y = (x_ref * sin(ref_yaw) + y_ref * cos(ref_yaw));
            x += ref_x;
            y += ref_y;

            // should contain 50 points trajectory
            next_x_vals.push_back(x);
            next_y_vals.push_back(y);
          }
          
          // initialize Json message containing the trajectory
          json msgJson;
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }  // end "telemetry" if
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }  // end websocket if
  }); // end h.onMessage

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  
  h.run();
}