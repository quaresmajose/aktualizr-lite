#include <limits>
#include <random>

#include <boost/optional.hpp>
#include <boost/algorithm/hex.hpp>
#include "libaktualizr/crypto/crypto.h"


namespace fixtures {

class Image {
  public:
   struct HashedData {
     HashedData(const std::string& d):data{d},
                                      hash{boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(d)))},
                                      size{data.size()} {}

     std::string data;
     std::string hash;
     std::size_t size;
   };

   Image(const std::string& name):name_{name}, layer_blob_{Utils::randomUuid()}, manifest_str_{"undefined"} {
     manifest_["mediaType"] = "application/vnd.docker.distribution.manifest.v2+json";
     manifest_["schemaVersion"] = 2;

     manifest_["config"]["mediaType"] = "application/vnd.docker.container.image.v1+json";
     manifest_["config"]["size"] = image_config_.size;
     manifest_["config"]["digest"] = "sha256:" + image_config_.hash;

     manifest_["layers"][0]["mediaType"] = "application/vnd.docker.image.rootfs.diff.tar.gzip";
     manifest_["layers"][0]["size"] = layer_blob_.size;
     manifest_["layers"][0]["digest"] = "sha256:" + layer_blob_.hash;
     manifest_str_ = HashedData{Utils::jsonToCanonicalStr(manifest_)};
     uri_ = name_ + "@sha256:" + manifest_str_.hash;
   }

   const std::string& name() const { return name_; }
   const HashedData& layerBlob() const { return layer_blob_; }
   const HashedData& config() const { return image_config_; }
   const HashedData& manifest() const { return manifest_str_; }
   std::string uri(const std::string host = "localhost") const { return host + "/" + uri_; }

  private:
   const std::string name_;
   const HashedData layer_blob_;
   const HashedData image_config_{"{}"};

   Json::Value manifest_;
   HashedData manifest_str_;
   std::string uri_;
};

class ComposeApp {
 public:
  using Ptr = std::shared_ptr<ComposeApp>;
  static constexpr const char* const DefaultTemplate = R"(
    services:
      %s
        labels:
          io.compose-spec.config-hash: %s
    x-fault-injection:
      failure-type: %s
    version: "3.8"
    )";

  static constexpr const char* const ServiceTemplate = R"(
      %s:
        image: %s)";

 public:
  static Ptr create(const std::string& name,
                    const std::string& service = "service-01", const std::string& image_name = "factory/image-01",
                    const std::string& service_template = ServiceTemplate,
                    const std::string& compose_file = Docker::ComposeAppEngine::ComposeFile,
                    const std::string& failure = "none",
                    const Json::Value& layers = Json::Value()) {
    Ptr app{new ComposeApp(name, compose_file, image_name)};

    // layers manifest
    Json::Value layers_json{layers};
    if (layers_json.isNull()) {
      std::random_device rand_dev;
      std::uniform_int_distribution<std::int64_t> dist{1024, std::numeric_limits<std::uint32_t>::max()};
      for (int ii = 0; ii < 3; ++ii) {
        layers_json["layers"][ii]["digest"] = "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
        layers_json["layers"][ii]["size"] = dist(rand_dev);
      }
    }

    app->updateService(service, service_template, failure, layers_json);
    return app;
  }

  static Ptr createAppWithCustomeLayers(const std::string& name, const Json::Value& layers, boost::optional<std::size_t> layer_man_size = boost::none) {
    Ptr app{new ComposeApp(name, Docker::ComposeAppEngine::ComposeFile, "factory/image-01")};
    app->updateService("service-01", ServiceTemplate, "none", layers, layer_man_size);
    return app;
  }

  const std::string& updateService(const std::string& service, const std::string& service_template = ServiceTemplate, const std::string& failure = "none", const Json::Value& layers = Json::Value(), boost::optional<std::size_t> layer_man_size = boost::none) {
    char service_content[1024];
    sprintf(service_content, service_template.c_str(), service.c_str(), image_.uri().c_str());
    auto service_hash = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(service_content)));
    sprintf(content_, DefaultTemplate, service_content, service_hash.c_str(), failure.c_str());
    return update(layers, layer_man_size);
  }

  const std::string& name() const { return name_; }
  const std::string& hash() const { return hash_; }
  const std::string& archHash() const { return arch_hash_; }
  const std::string& archive() const { return arch_; }
  const std::string& manifest() const { return manifest_; }
  const Image& image() const { return image_; }
  const std::string& layersManifest() const { return layers_manifest_; }
  const std::string& layersHash() const { return layers_hash_; }


 private:
  ComposeApp(const std::string& name, const std::string& compose_file, const std::string& image_name):compose_file_{compose_file}, name_{name}, image_{image_name} {}

  const std::string& update(const Json::Value& layers = Json::Value(), boost::optional<std::size_t> layer_man_size = boost::none) {
    TemporaryDirectory app_dir;
    TemporaryFile arch_file{"arch.tgz"};

    Utils::writeFile(app_dir.Path() / compose_file_, std::string(content_));
    auto cmd = std::string("tar -czf ") + arch_file.Path().string() + " " + compose_file_;
    if (0 != boost::process::system(cmd, boost::process::start_dir = app_dir.Path())) {
      throw std::runtime_error("failed to create App archive: " + name());
    }
    arch_ = Utils::readFile(arch_file.Path());
    arch_hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(arch_)));

    Json::Value manifest;
    manifest["annotations"]["compose-app"] = "v1";
    manifest["layers"][0]["digest"] = "sha256:" + arch_hash_;
    manifest["layers"][0]["size"] = arch_.size();

    // layers manifest
    Json::Value layers_json{layers};
    if (layers_json.isNull()) {
      std::random_device rand_dev;
      std::uniform_int_distribution<std::int64_t> dist{1024, std::numeric_limits<std::uint32_t>::max()};
      for (int ii = 0; ii < 3; ++ii) {
        layers_json["layers"][ii]["digest"] = "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
        layers_json["layers"][ii]["size"] = dist(rand_dev);
      }
    }

    if (!layers.isNull()) {
      layers_manifest_ = Utils::jsonToCanonicalStr(layers_json);
      layers_hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(layers_manifest_)));

      // update the App manifest with metadata about the layers' manifest
      manifest["manifests"][0]["mediaType"] = "application/vnd.docker.distribution.manifest.v2+json";
      if (!!layer_man_size) {
        manifest["manifests"][0]["size"] = *layer_man_size;
      } else {
        manifest["manifests"][0]["size"] = layers_manifest_.size();
      }
      manifest["manifests"][0]["digest"] = "sha256:" + layers_hash_;
      manifest["manifests"][0]["platform"]["architecture"] = "amd64";
      manifest["manifests"][0]["platform"]["os"] = "linux";
    }
    // emulate compose-publish work, i.e. calculate hash on a manifest json as it is,
    // no need to normalize it to a canonical representation
    manifest_ = Utils::jsonToStr(manifest);
    hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(manifest_)));
    return hash_;
  }

 private:
  const std::string compose_file_;
  const std::string name_;
  const Image image_;
  char content_[4096];

  std::string arch_;
  std::string arch_hash_;
  std::string manifest_;
  std::string hash_;
  std::string layers_manifest_;
  std::string layers_hash_;
};


} // namespace fixtures
