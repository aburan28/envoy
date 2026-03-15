#include "source/extensions/filters/http/admission_control/admission_control.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "envoy/common/random_generator.h"
#include "envoy/extensions/filters/http/admission_control/v3/admission_control.pb.h"
#include "envoy/grpc/status.h"
#include "envoy/http/codes.h"
#include "envoy/runtime/runtime.h"
#include "envoy/server/filter_config.h"

#include "source/common/common/cleanup.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codes.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/http/admission_control/evaluators/success_criteria_evaluator.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AdmissionControl {

using GrpcStatus = Grpc::Status::GrpcStatus;

static constexpr double defaultAggression = 1.0;
static constexpr double defaultSuccessRateThreshold = 95.0;
static constexpr uint32_t defaultRpsThreshold = 0;
static constexpr double defaultMaxRejectionProbability = 80.0;

AdmissionControlFilterConfig::AdmissionControlFilterConfig(
    const AdmissionControlProto& proto_config, Runtime::Loader& runtime,
    Random::RandomGenerator& random, Stats::Scope& scope,
    ThreadLocal::TypedSlotPtr<ThreadLocalControllerImpl>&& tls,
    std::shared_ptr<ResponseEvaluator> response_evaluator)
    : random_(random), scope_(scope), tls_(std::move(tls)),
      admission_control_feature_(proto_config.enabled(), runtime),
      aggression_(proto_config.has_aggression()
                      ? std::make_unique<Runtime::Double>(proto_config.aggression(), runtime)
                      : nullptr),
      sr_threshold_(proto_config.has_sr_threshold() ? std::make_unique<Runtime::Percentage>(
                                                          proto_config.sr_threshold(), runtime)
                                                    : nullptr),
      rps_threshold_(proto_config.has_rps_threshold()
                         ? std::make_unique<Runtime::UInt32>(proto_config.rps_threshold(), runtime)
                         : nullptr),
      max_rejection_probability_(proto_config.has_max_rejection_probability()
                                     ? std::make_unique<Runtime::Percentage>(
                                           proto_config.max_rejection_probability(), runtime)
                                     : nullptr),
      response_evaluator_(std::move(response_evaluator)) {}

double AdmissionControlFilterConfig::aggression() const {
  return std::max<double>(1.0, aggression_ ? aggression_->value() : defaultAggression);
}

double AdmissionControlFilterConfig::successRateThreshold() const {
  const double pct = sr_threshold_ ? sr_threshold_->value() : defaultSuccessRateThreshold;
  return std::min<double>(pct, 100.0) / 100.0;
}

uint32_t AdmissionControlFilterConfig::rpsThreshold() const {
  return rps_threshold_ ? rps_threshold_->value() : defaultRpsThreshold;
}

double AdmissionControlFilterConfig::maxRejectionProbability() const {
  const double ret = max_rejection_probability_ ? max_rejection_probability_->value()
                                                : defaultMaxRejectionProbability;
  return ret / 100.0;
}

// --- Per-route config ---

AdmissionControlPerRouteFilterConfig::AdmissionControlPerRouteFilterConfig(
    const AdmissionControlPerRouteProto& proto_config, Runtime::Loader& runtime,
    std::shared_ptr<ResponseEvaluator> response_evaluator)
    : disabled_(proto_config.has_disabled()) {
  if (!disabled_) {
    const auto& ac = proto_config.admission_control();
    if (ac.has_enabled()) {
      admission_control_feature_ =
          std::make_unique<Runtime::FeatureFlag>(ac.enabled(), runtime);
    }
    if (ac.has_aggression()) {
      aggression_ = std::make_unique<Runtime::Double>(ac.aggression(), runtime);
    }
    if (ac.has_sr_threshold()) {
      sr_threshold_ = std::make_unique<Runtime::Percentage>(ac.sr_threshold(), runtime);
    }
    if (ac.has_rps_threshold()) {
      rps_threshold_ = std::make_unique<Runtime::UInt32>(ac.rps_threshold(), runtime);
    }
    if (ac.has_max_rejection_probability()) {
      max_rejection_probability_ =
          std::make_unique<Runtime::Percentage>(ac.max_rejection_probability(), runtime);
    }
    response_evaluator_ = std::move(response_evaluator);
  }
}

bool AdmissionControlPerRouteFilterConfig::filterEnabled() const {
  if (disabled_) {
    return false;
  }
  return admission_control_feature_ ? admission_control_feature_->enabled() : true;
}

double AdmissionControlPerRouteFilterConfig::aggression() const {
  return std::max<double>(1.0, aggression_ ? aggression_->value() : defaultAggression);
}

double AdmissionControlPerRouteFilterConfig::successRateThreshold() const {
  const double pct = sr_threshold_ ? sr_threshold_->value() : defaultSuccessRateThreshold;
  return std::min<double>(pct, 100.0) / 100.0;
}

uint32_t AdmissionControlPerRouteFilterConfig::rpsThreshold() const {
  return rps_threshold_ ? rps_threshold_->value() : defaultRpsThreshold;
}

double AdmissionControlPerRouteFilterConfig::maxRejectionProbability() const {
  const double ret = max_rejection_probability_ ? max_rejection_probability_->value()
                                                : defaultMaxRejectionProbability;
  return ret / 100.0;
}

// --- Filter ---

AdmissionControlFilter::AdmissionControlFilter(AdmissionControlFilterConfigSharedPtr config,
                                               const std::string& stats_prefix)
    : config_(std::move(config)), stats_(generateStats(config_->scope(), stats_prefix)) {}

bool AdmissionControlFilter::resolvedFilterEnabled() const {
  if (per_route_config_) {
    return per_route_config_->filterEnabled();
  }
  return config_->filterEnabled();
}

double AdmissionControlFilter::resolvedAggression() const {
  if (per_route_config_ && !per_route_config_->disabled()) {
    return per_route_config_->aggression();
  }
  return config_->aggression();
}

double AdmissionControlFilter::resolvedSuccessRateThreshold() const {
  if (per_route_config_ && !per_route_config_->disabled()) {
    return per_route_config_->successRateThreshold();
  }
  return config_->successRateThreshold();
}

uint32_t AdmissionControlFilter::resolvedRpsThreshold() const {
  if (per_route_config_ && !per_route_config_->disabled()) {
    return per_route_config_->rpsThreshold();
  }
  return config_->rpsThreshold();
}

double AdmissionControlFilter::resolvedMaxRejectionProbability() const {
  if (per_route_config_ && !per_route_config_->disabled()) {
    return per_route_config_->maxRejectionProbability();
  }
  return config_->maxRejectionProbability();
}

ResponseEvaluator& AdmissionControlFilter::resolvedResponseEvaluator() const {
  if (per_route_config_ && !per_route_config_->disabled() &&
      per_route_config_->responseEvaluator() != nullptr) {
    return *per_route_config_->responseEvaluator();
  }
  return config_->responseEvaluator();
}

Http::FilterHeadersStatus AdmissionControlFilter::decodeHeaders(Http::RequestHeaderMap&, bool) {
  per_route_config_ =
      Http::Utility::resolveMostSpecificPerFilterConfig<AdmissionControlPerRouteFilterConfig>(
          decoder_callbacks_);

  if (!resolvedFilterEnabled() || decoder_callbacks_->streamInfo().healthCheck()) {
    record_request_ = false;
    return Http::FilterHeadersStatus::Continue;
  }

  if (config_->getController().averageRps() < resolvedRpsThreshold()) {
    ENVOY_LOG(debug, "Current rps: {} is below rps_threshold: {}, continue",
              config_->getController().averageRps(), resolvedRpsThreshold());
    return Http::FilterHeadersStatus::Continue;
  }

  if (shouldRejectRequest()) {
    record_request_ = false;

    stats_.rq_rejected_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::ServiceUnavailable, "", nullptr, absl::nullopt,
                                       "denied_by_admission_control");
    return Http::FilterHeadersStatus::StopIteration;
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus AdmissionControlFilter::encodeHeaders(Http::ResponseHeaderMap& headers,
                                                                bool end_stream) {
  if (!record_request_) {
    return Http::FilterHeadersStatus::Continue;
  }

  bool successful_response = false;
  const auto& evaluator = resolvedResponseEvaluator();
  if (Grpc::Common::isGrpcResponseHeaders(headers, end_stream)) {
    absl::optional<GrpcStatus> grpc_status = Grpc::Common::getGrpcStatus(headers);

    expect_grpc_status_in_trailer_ = !grpc_status.has_value();
    if (expect_grpc_status_in_trailer_) {
      return Http::FilterHeadersStatus::Continue;
    }

    const uint32_t status = enumToInt(grpc_status.value());
    successful_response = evaluator.isGrpcSuccess(status);
  } else {
    const uint64_t http_status = Http::Utility::getResponseStatus(headers);
    successful_response = evaluator.isHttpSuccess(http_status);
  }

  if (successful_response) {
    recordSuccess();
  } else {
    recordFailure();
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterTrailersStatus
AdmissionControlFilter::encodeTrailers(Http::ResponseTrailerMap& trailers) {
  if (expect_grpc_status_in_trailer_) {
    absl::optional<GrpcStatus> grpc_status = Grpc::Common::getGrpcStatus(trailers, false);
    const auto& evaluator = resolvedResponseEvaluator();

    if (grpc_status.has_value() && evaluator.isGrpcSuccess(grpc_status.value())) {
      recordSuccess();
    } else {
      recordFailure();
    }
  }

  return Http::FilterTrailersStatus::Continue;
}

bool AdmissionControlFilter::shouldRejectRequest() const {
  const auto request_counts = config_->getController().requestCounts();
  const double total_requests = request_counts.requests;
  const double successful_requests = request_counts.successes;
  double probability =
      total_requests - successful_requests / resolvedSuccessRateThreshold();
  probability = probability / (total_requests + 1);
  const auto aggression = resolvedAggression();
  if (aggression != 1.0) {
    probability = std::pow(probability, 1.0 / aggression);
  }
  probability = std::min<double>(probability, resolvedMaxRejectionProbability());

  static constexpr uint64_t accuracy = 1e4;
  auto r = config_->random().random();
  return (accuracy * std::max(probability, 0.0)) > (r % accuracy);
}

} // namespace AdmissionControl
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
