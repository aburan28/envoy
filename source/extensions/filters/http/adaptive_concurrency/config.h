#pragma once

#include "envoy/extensions/filters/http/adaptive_concurrency/v3/adaptive_concurrency.pb.h"
#include "envoy/extensions/filters/http/adaptive_concurrency/v3/adaptive_concurrency.pb.validate.h"

#include "source/extensions/filters/http/common/factory_base.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AdaptiveConcurrency {

/**
 * Config registration for the adaptive concurrency limit filter. @see NamedHttpFilterConfigFactory.
 * Supports both downstream and upstream HTTP filter chains via DualFactoryBase.
 */
class AdaptiveConcurrencyFilterFactory
    : public Common::DualFactoryBase<
          envoy::extensions::filters::http::adaptive_concurrency::v3::AdaptiveConcurrency> {
public:
  AdaptiveConcurrencyFilterFactory()
      : DualFactoryBase("envoy.filters.http.adaptive_concurrency") {}

private:
  absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::adaptive_concurrency::v3::AdaptiveConcurrency&
          proto_config,
      const std::string& stats_prefix, DualInfo info,
      Server::Configuration::ServerFactoryContext& context) override;

  Envoy::Http::FilterFactoryCb createFilterFactoryFromProtoWithServerContextTyped(
      const envoy::extensions::filters::http::adaptive_concurrency::v3::AdaptiveConcurrency&
          proto_config,
      const std::string& stats_prefix,
      Server::Configuration::ServerFactoryContext& context) override;
};

using UpstreamAdaptiveConcurrencyFilterFactory = AdaptiveConcurrencyFilterFactory;

DECLARE_FACTORY(AdaptiveConcurrencyFilterFactory);
DECLARE_FACTORY(UpstreamAdaptiveConcurrencyFilterFactory);

} // namespace AdaptiveConcurrency
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
