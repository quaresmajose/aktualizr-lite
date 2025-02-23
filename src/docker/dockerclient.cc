#include "dockerclient.h"
#include <boost/format.hpp>
#include <boost/process.hpp>
#include "http/httpclient.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace Docker {

const DockerClient::HttpClientFactory DockerClient::DefaultHttpClientFactory = [](const std::string& docker_host_in) {
  std::string docker_host{docker_host_in};
  auto env{boost::this_process::environment()};
  if (env.end() != env.find("DOCKER_HOST")) {
    docker_host = env.get("DOCKER_HOST");
  }
  static const std::string docker_host_prefix{"unix://"};
  const auto find_res = docker_host.find_first_of(docker_host_prefix);
  if (find_res != 0) {
    throw std::invalid_argument("Invalid docker host value, must start with unix:// : " + docker_host);
  }

  const auto socket{docker_host.substr(docker_host_prefix.size())};
  return std::make_shared<HttpClient>(socket);
};

DockerClient::DockerClient(std::shared_ptr<HttpInterface> http_client)
    : http_client_{std::move(http_client)},
      engine_info_{getEngineInfo()},
      arch_{engine_info_.get("Arch", Json::Value()).asString()} {}

void DockerClient::getContainers(Json::Value& root) {
  // curl --unix-socket /var/run/docker.sock http://localhost/containers/json?all=1
  const std::string cmd{"http://localhost/containers/json?all=1"};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (resp.isOk()) {
    root = resp.getJson();
  }
  if (!root) {
    // check if the `root` json is initialized, not `empty()` since dockerd can return 200/OK with
    // empty json `[]`, which is not exceptional situation and means zero containers are running
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
}

std::tuple<bool, std::string> DockerClient::getContainerState(const Json::Value& root, const std::string& app,
                                                              const std::string& service,
                                                              const std::string& hash) const {
  for (Json::ValueConstIterator ii = root.begin(); ii != root.end(); ++ii) {
    Json::Value val = *ii;
    if (val["Labels"]["com.docker.compose.project"].asString() == app) {
      if (val["Labels"]["com.docker.compose.service"].asString() == service) {
        if (val["Labels"]["io.compose-spec.config-hash"].asString() == hash) {
          return {true, val["State"].asString()};
        }
      }
    }
  }
  return {false, ""};
}

Json::Value DockerClient::getContainerInfo(const std::string& id) {
  const std::string cmd{"http://localhost/containers/" + id + "/json"};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (!resp.isOk()) {
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
  return resp.getJson();
}

std::string DockerClient::getContainerLogs(const std::string& id, int tail) {
  const std::string cmd{"http://localhost/containers/" + id + "/logs?stderr=1&tail=" + std::to_string(tail)};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (!resp.isOk()) {
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
  return resp.body;
}

Json::Value DockerClient::getRunningApps(const std::function<void(const std::string&, Json::Value&)>& ext_func) {
  Json::Value apps;
  Json::Value containers;
  getContainers(containers);

  for (Json::ValueIterator ii = containers.begin(); ii != containers.end(); ++ii) {
    Json::Value val = *ii;

    std::string app_name = val["Labels"]["com.docker.compose.project"].asString();
    if (app_name.empty()) {
      continue;
    }

    std::string state = val["State"].asString();
    std::string status = val["Status"].asString();

    Json::Value service_attributes;
    service_attributes["name"] = val["Labels"]["com.docker.compose.service"].asString();
    service_attributes["hash"] = val["Labels"]["io.compose-spec.config-hash"].asString();
    service_attributes["image"] = val["Image"].asString();
    service_attributes["state"] = state;
    service_attributes["status"] = status;

    // (created|restarting|running|removing|paused|exited|dead)
    service_attributes["health"] = "healthy";
    if (status.find("health") != std::string::npos) {
      service_attributes["health"] = getContainerInfo(val["Id"].asString())["State"]["Health"]["Status"].asString();
    } else {
      if (state == "dead" ||
          (state == "exited" && getContainerInfo(val["Id"].asString())["State"]["ExitCode"].asInt() != 0)) {
        service_attributes["health"] = "unhealthy";
      }
    }

    if (service_attributes["health"] != "healthy") {
      service_attributes["logs"] = getContainerLogs(val["Id"].asString(), 5);
    }

    apps[app_name]["services"].append(service_attributes);

    if (ext_func) {
      ext_func(app_name, apps[app_name]);
    }
  }
  return apps;
}

Json::Value DockerClient::getEngineInfo() {
  Json::Value info;
  const std::string cmd{"http://localhost/version"};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (resp.isOk()) {
    info = resp.getJson();
  }
  if (!info) {
    // check if the `root` json is initialized, not `empty()` since dockerd can return 200/OK with
    // empty json `[]`, which is not exceptional situation and means zero containers are running
    throw std::runtime_error("Request to the dockerd's /version endpoint has failed: " + cmd);
  }
  return info;
}

}  // namespace Docker
