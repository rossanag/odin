#include <functional>
#include <string>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sstream>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <prime_server/prime_server.hpp>
#include <prime_server/http_protocol.hpp>
using namespace prime_server;

#include <valhalla/midgard/logging.h>

#include "proto/directions_options.pb.h"
#include "proto/trippath.pb.h"
#include "odin/service.h"
#include "odin/util.h"
#include "odin/directionsbuilder.h"

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;

namespace {
  const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};

  //TODO: throw this in the header to make it testable?
  class odin_worker_t {
   public:
    odin_worker_t(const boost::property_tree::ptree& config):config(config) {

    }
    worker_t::result_t work(const std::list<zmq::message_t>& job, void* request_info) {
      auto& info = *static_cast<http_request_t::info_t*>(request_info);
      LOG_INFO("Got Odin Request " + std::to_string(info.id));
      try{
        //crack open the original request
        std::string request_str(static_cast<const char*>(job.front().data()), job.front().size());
        std::stringstream stream(request_str);
        boost::property_tree::ptree request;
        try{
          boost::property_tree::read_json(stream, request);
        }
        catch(...) {
          worker_t::result_t result{false};
          http_response_t response(500, "Internal Server Error", "Failed to parse intermediate request format", headers_t{CORS});
          response.from_info(info);
          result.messages.emplace_back(response.to_string());
          return result;
        }

        // Grab language from options and set
        auto language = request.get_optional<std::string>("directions_options.language");
        // If language is not found then set to the default language (en-US)
        if (!language || (odin::get_locales().find(*language) == odin::get_locales().end())) {
          request.put<std::string>("directions_options.language", odin::DirectionsOptions::default_instance().language());
          std::stringstream ss;
          boost::property_tree::write_json(ss, request, false);
          // Update request string with language
          request_str = ss.str();
        }

        //see if we can get some options
        valhalla::odin::DirectionsOptions directions_options;
        auto options = request.get_child_optional("directions_options");
        if(options)
          directions_options = valhalla::odin::GetDirectionsOptions(*options);

        //forward the original request
        worker_t::result_t result{true};
        result.messages.emplace_back(std::move(request_str));

        //for each leg
        for(auto leg = ++job.cbegin(); leg != job.cend(); ++leg) {
          //crack open the path
          odin::TripPath trip_path;
          try {
            trip_path.ParseFromArray(leg->data(), static_cast<int>(leg->size()));
          }
          catch(...) {
            worker_t::result_t result{false};
            http_response_t response(500, "Internal Server Error", "Failed to parse TripPath", headers_t{CORS});
            response.from_info(info);
            result.messages.emplace_back(response.to_string());
            return result;
          }

          //get some annotated directions
          odin::DirectionsBuilder directions;
          odin::TripDirections trip_directions;
          try{
            trip_directions = directions.Build(directions_options, trip_path);
          }
          catch(...) {
            worker_t::result_t result{false};
            http_response_t response(500, "Internal Server Error", "Could not build directions for TripPath", headers_t{CORS});
            response.from_info(info);
            result.messages.emplace_back(response.to_string());
            return result;
          }

          LOG_INFO("maneuver_count::" + std::to_string(trip_directions.maneuver_size()));

          //the protobuf directions
          result.messages.emplace_back(trip_directions.SerializeAsString());
        }

        return result;
      }
      catch(const std::exception& e) {
        worker_t::result_t result{false};
        http_response_t response(400, "Bad Request", e.what(), headers_t{CORS});
        response.from_info(info);
        result.messages.emplace_back(response.to_string());
        return result;
      }
    }
   protected:
    boost::property_tree::ptree config;
  };
}

namespace valhalla {
  namespace odin {
    void run_service(const boost::property_tree::ptree& config) {
      //gets requests from odin proxy
      auto upstream_endpoint = config.get<std::string>("odin.service.proxy") + "_out";
      //sends them on to tyr
      auto downstream_endpoint = config.get<std::string>("tyr.service.proxy") + "_in";
      //or returns just location information back to the server
      auto loopback_endpoint = config.get<std::string>("httpd.service.loopback");

      //listen for requests
      zmq::context_t context;
      prime_server::worker_t worker(context, upstream_endpoint, downstream_endpoint, loopback_endpoint,
        std::bind(&odin_worker_t::work, odin_worker_t(config), std::placeholders::_1, std::placeholders::_2));
      worker.work();

      //TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
    }
  }
}
