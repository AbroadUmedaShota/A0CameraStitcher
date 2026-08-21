#include "a0/m2/stitch_metrics.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("schema fixture cannot be opened");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string ReplaceOnce(std::string value, const std::string& from, const std::string& to) {
    const auto position = value.find(from);
    if (position == std::string::npos) throw std::runtime_error("test replacement marker missing");
    value.replace(position, from.size(), to);
    return value;
}

std::string ImageDefinitionJson(
    const std::string& rounding_mode = "HalfToEven",
    const int decimal_places = 2) {
    return std::string(R"({"schemaVersion":"a0.stitch-metric-definition.v1","definitionVersion":1,"metricId":"seam-registration-error","domain":"Image","unit":"pixels","coordinateSystem":{"origin":"TopLeftPixelCenter","xAxis":"Right","yAxis":"Down","reference":"StitchedOutput"},"mask":{"kind":"BinaryValidity","source":"IntersectionOfValidSamples","includedValue":1,"excludedValue":0},"sampling":{"method":"Bilinear","outOfBounds":"Exclude","invalidSample":"Exclude"},"aggregation":{"method":"Percentile","percentile":95,"algorithm":"SortedOrderStatistic","percentileInterpolation":"LinearR7","medianEvenRule":null},"rounding":{"mode":")") +
        rounding_mode + R"(","decimalPlaces":)" + std::to_string(decimal_places) +
        R"(,"order":"AggregateThenRound"},"boundary":{"imageEdge":"ExcludeIncompleteKernel","maskEdge":"RequireAllSamplesValid","intervalClosure":"LowerInclusiveUpperExclusive"},"confidence":{"method":"BootstrapPercentile","level":0.95},"uncertainty":{"method":"StandardError","unitMode":"SameAsMetric"},"outcomePolicy":{"allowedFailureCodes":{"NoResult":["insufficient-valid-samples"],"NotApplicable":[],"Invalid":["invalid-source-contract"]},"noResultRule":"ZeroValidSamples"}})";
}

std::string ResourceDefinitionJson() {
    return R"({"schemaVersion":"a0.stitch-metric-definition.v1","definitionVersion":1,"metricId":"capture-duration","domain":"Resource","unit":"milliseconds","coordinateSystem":null,"mask":null,"sampling":null,"aggregation":{"method":"Mean","percentile":null,"algorithm":"PairwiseArithmeticMean","percentileInterpolation":null,"medianEvenRule":null},"rounding":{"mode":"HalfAwayFromZero","decimalPlaces":2,"order":"AggregateThenRound"},"boundary":null,"confidence":{"method":"None","level":null},"uncertainty":{"method":"StandardDeviation","unitMode":"SameAsMetric"},"outcomePolicy":{"allowedFailureCodes":{"NoResult":["zero-valid-results"],"NotApplicable":["not-applicable-for-metric-domain"],"Invalid":["invalid-source-contract"]},"noResultRule":"ZeroValidSamples"}})";
}

std::string MedianResourceDefinitionJson() {
    auto definition = ReplaceOnce(ResourceDefinitionJson(), "\"method\":\"Mean\"", "\"method\":\"Median\"");
    definition = ReplaceOnce(definition, "\"algorithm\":\"PairwiseArithmeticMean\"", "\"algorithm\":\"SortedMiddle\"");
    return ReplaceOnce(definition, "\"medianEvenRule\":null", "\"medianEvenRule\":\"MeanOfMiddlePair\"");
}

std::string SuccessResultJson(
    const std::string& domain = "Image",
    const std::string& metric_id = "seam-registration-error",
    const std::string& rounding_mode = "HalfToEven",
    const std::string& raw_value = "1.245",
    const std::string& rounded_value = "1.24",
    const int decimal_places = 2,
    const std::string& lower_bound = "1.10",
    const std::string& upper_bound = "1.40") {
    return std::string(R"({"schemaVersion":"a0.stitch-metric-result.v1","definitionVersion":1,"metricId":")") +
        metric_id + R"(","metricDomain":")" + domain +
        R"(","outcome":"Success","rawValue":)" + raw_value + R"(,"value":)" + rounded_value +
        R"(,"sampleCount":128,"rounding":{"mode":")" + rounding_mode +
        R"(","decimalPlaces":)" + std::to_string(decimal_places) +
        R"(,"order":"AggregateThenRound"},"confidence":{"method":"BootstrapPercentile","level":0.95,"lower":)" +
        lower_bound + R"(,"upper":)" + upper_bound +
        R"(},"uncertainty":{"method":"StandardError","value":0.05},"failureCode":null})";
}

std::string NonSuccessResultJson(
    const std::string& domain,
    const std::string& metric_id,
    const std::string& outcome,
    const std::string& code,
    const std::string& rounding_mode = "HalfToEven") {
    return std::string(R"({"schemaVersion":"a0.stitch-metric-result.v1","definitionVersion":1,"metricId":")") +
        metric_id + R"(","metricDomain":")" + domain + R"(","outcome":")" + outcome +
        R"(","rawValue":null,"value":null,"sampleCount":0,"rounding":{"mode":")" + rounding_mode +
        R"(","decimalPlaces":2,"order":"AggregateThenRound"},"confidence":{"method":"None","level":null,"lower":null,"upper":null},"uncertainty":{"method":"None","value":null},"failureCode":")" +
        code + R"("})";
}

std::string StatisticalResultJson(
    const std::string& raw_value = "1.305",
    const std::string& rounded_value = "1.30",
    const int decimal_places = 2,
    const std::string& lower_bound = "1.10",
    const std::string& upper_bound = "1.50") {
    return std::string(R"({"schemaVersion":"a0.stitch-statistical-result.v1","definitionVersion":1,"metricId":"seam-registration-error","metricDomain":"Image","outcome":"Success","aggregation":{"method":"Percentile","percentile":95,"algorithm":"SortedOrderStatistic","percentileInterpolation":"LinearR7","medianEvenRule":null},"populationCount":100,"validResultCount":98,"rawValue":)") +
        raw_value + R"(,"value":)" + rounded_value +
        R"(,"rounding":{"mode":"HalfToEven","decimalPlaces":)" + std::to_string(decimal_places) +
        R"(,"order":"AggregateThenRound"},"confidence":{"method":"BootstrapPercentile","level":0.95,"lower":)" +
        lower_bound + R"(,"upper":)" + upper_bound +
        R"(},"uncertainty":{"method":"ConfidenceIntervalHalfWidth","value":0.20},"failureCode":null})";
}

std::string StatisticalNoResultJson() {
    auto result = ReplaceOnce(StatisticalResultJson(), "\"outcome\":\"Success\"", "\"outcome\":\"NoResult\"");
    result = ReplaceOnce(result, "\"validResultCount\":98", "\"validResultCount\":0");
    result = ReplaceOnce(result, "\"rawValue\":1.305", "\"rawValue\":null");
    result = ReplaceOnce(result, "\"value\":1.30", "\"value\":null");
    result = ReplaceOnce(result,
        "\"confidence\":{\"method\":\"BootstrapPercentile\",\"level\":0.95,\"lower\":1.10,\"upper\":1.50}",
        "\"confidence\":{\"method\":\"None\",\"level\":null,\"lower\":null,\"upper\":null}");
    result = ReplaceOnce(result,
        "\"uncertainty\":{\"method\":\"ConfidenceIntervalHalfWidth\",\"value\":0.20},\"failureCode\":null",
        "\"uncertainty\":{\"method\":\"None\",\"value\":null},\"failureCode\":\"insufficient-valid-samples\"");
    return result;
}

void CheckRejected(
    const a0::m2::StitchMetricValidation& result,
    const std::string& message) {
    Check(!result.valid && !result.failure_code.empty() && result.failure_code.size() <= 64,
        message);
}

std::string QuotePowerShellLiteral(std::string value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char character : value) {
        quoted.push_back(character);
        if (character == '\'') quoted.push_back('\'');
    }
    quoted.push_back('\'');
    return quoted;
}

bool CheckedInSchemaAccepts(
    const std::filesystem::path& schema_path,
    const std::string& json) {
    static std::atomic<unsigned long> sequence = 0;
    const auto fixture_path = std::filesystem::temp_directory_path() /
        ("a0-stitch-metric-schema-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(sequence.fetch_add(1)) + ".json");
    {
        std::ofstream output(fixture_path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("temporary schema fixture cannot be created");
        output << json;
    }
    const std::string command =
        "pwsh -NoProfile -NonInteractive -Command \"$ErrorActionPreference='SilentlyContinue'; "
        "if (Test-Json -LiteralPath " + QuotePowerShellLiteral(fixture_path.string()) +
        " -SchemaFile " + QuotePowerShellLiteral(schema_path.string()) +
        " -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }\"";
    const int exit_code = std::system(command.c_str());
    std::error_code ignored;
    std::filesystem::remove(fixture_path, ignored);
    return exit_code == 0;
}

using Validator = std::function<a0::m2::StitchMetricValidation(const std::string&)>;

void CheckSchemaRuntimeMatrix(
    const std::filesystem::path& schema_path,
    const Validator& validator,
    const std::vector<std::pair<std::string, bool>>& fixtures,
    const std::string& label) {
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        const bool schema_accepts = CheckedInSchemaAccepts(schema_path, fixtures[index].first);
        const bool runtime_accepts = validator(fixtures[index].first).valid;
        Check(schema_accepts == fixtures[index].second,
            label + " checked-in schema expectation mismatch at fixture " + std::to_string(index));
        Check(runtime_accepts == fixtures[index].second,
            label + " runtime expectation mismatch at fixture " + std::to_string(index));
        Check(schema_accepts == runtime_accepts,
            label + " schema/runtime drift at fixture " + std::to_string(index));
    }
}

void TestCheckedInSchemasDirectlyValidateFixtures() {
#ifdef A0_SOURCE_ROOT
    const auto root = std::filesystem::path(A0_SOURCE_ROOT) / "docs" / "schemas";
    const auto definition_schema = root / "stitch-metric-definition.schema.json";
    const auto result_schema = root / "stitch-metric-result.schema.json";
    const auto statistical_schema = root / "stitch-statistical-result.schema.json";
    for (const auto& schema_path : {definition_schema, result_schema, statistical_schema}) {
        const auto schema = ReadText(schema_path);
        Check(a0::m2::IsStitchMetricJsonSyntaxValid(schema),
            schema_path.filename().string() + " must parse as JSON");
        Check(schema.find("threshold") == std::string::npos &&
                schema.find("Threshold") == std::string::npos,
            schema_path.filename().string() + " must contain zero business/product thresholds");
    }

    const std::vector<std::pair<std::string, bool>> definitions = {
        {ImageDefinitionJson(), true},
        {ResourceDefinitionJson(), true},
        {ReplaceOnce(ImageDefinitionJson(), "\"definitionVersion\":1", "\"definitionVersion\":1e0"), true},
        {ReplaceOnce(ResourceDefinitionJson(), "\"unit\":\"milliseconds\"", "\"unit\":\"seconds\""), true},
        {ReplaceOnce(ResourceDefinitionJson(), "\"unit\":\"milliseconds\"", "\"unit\":\"bytes\""), true},
        {ReplaceOnce(ResourceDefinitionJson(), "\"unit\":\"milliseconds\"", "\"unit\":\"MiB\""), true},
        {MedianResourceDefinitionJson(), true},
        {ReplaceOnce(ImageDefinitionJson(), "definition.v1", "definition.v2"), false},
        {ReplaceOnce(ImageDefinitionJson(), "\"definitionVersion\":1", "\"definitionVersion\":1.00000000000000001"), false},
        {ReplaceOnce(ImageDefinitionJson(), "\"definitionVersion\":1,", ""), false},
        {ReplaceOnce(ImageDefinitionJson(), "\"unit\":\"pixels\"", "\"unit\":\"quality-score\""), false},
        {ReplaceOnce(ResourceDefinitionJson(), "\"coordinateSystem\":null", "\"coordinateSystem\":{}"), false},
        {ReplaceOnce(ImageDefinitionJson(), "\"NotApplicable\":[]", "\"NotApplicable\":[\"not-applicable-for-metric-domain\"]"), false},
        {ReplaceOnce(ImageDefinitionJson(), "\"NoResult\":[\"insufficient-valid-samples\"]", "\"NoResult\":[\"arbitrary-token\"]"), false}};
    CheckSchemaRuntimeMatrix(
        definition_schema,
        [](const std::string& json) { return a0::m2::ValidateStitchMetricDefinitionJson(json); },
        definitions,
        "definition");

    const std::vector<std::pair<std::string, bool>> results = {
        {SuccessResultJson(), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "1e-400", "0", 0, "-1", "1"), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "5e-324", "0", 0, "-1", "1"), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "-0.0", "0", 0, "-1", "1"), true},
        {SuccessResultJson("Image", "seam-registration-error", "HalfToEven",
             "1.245e0", "1.24e0", 2, "0", "2"), true},
        {SuccessResultJson("Image", "seam-registration-error", "HalfToEven",
             "1.245", "1.24", 2, "0", "2"), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "549755813888", "549755813888", 0, "-549755813888", "549755813888"), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "-549755813888", "-549755813888", 0, "-549755813888", "549755813888"), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "549.755813888", "549.755813888", 9, "-549.755813888", "549.755813888"), true},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "549755813889", "549755813889", 0, "-549755813889", "549755813889"), false},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "-549755813889", "-549755813889", 0, "-549755813889", "549755813889"), false},
        {SuccessResultJson("Image", "seam-registration-error", "TowardZero",
             "549.7558138881", "549.7558138881", 9, "-549.7558138881", "549.7558138881"), false},
        {NonSuccessResultJson("Resource", "capture-duration", "NotApplicable",
             "not-applicable-for-metric-domain", "HalfAwayFromZero"), true},
        {ReplaceOnce(SuccessResultJson(), "result.v1", "result.v2"), false},
        {ReplaceOnce(SuccessResultJson(), "\"rawValue\":1.245,", ""), false},
        {ReplaceOnce(SuccessResultJson(), "\"order\":\"AggregateThenRound\"", "\"order\":\"RoundThenAggregate\""), false},
        {NonSuccessResultJson("Image", "seam-registration-error", "NotApplicable",
             "not-applicable-for-metric-domain"), false},
        {NonSuccessResultJson("Image", "seam-registration-error", "NoResult", "arbitrary-token"), false}};
    CheckSchemaRuntimeMatrix(
        result_schema,
        [&](const std::string& json) {
            const bool resource = json.find("\"metricDomain\":\"Resource\"") != std::string::npos;
            const int decimal_places = json.find("\"decimalPlaces\":0") != std::string::npos ? 0
                : json.find("\"decimalPlaces\":9") != std::string::npos ? 9 : 2;
            const std::string mode = json.find("\"mode\":\"TowardZero\"") != std::string::npos
                ? "TowardZero"
                : json.find("\"mode\":\"HalfAwayFromZero\"") != std::string::npos
                    ? "HalfAwayFromZero" : "HalfToEven";
            const auto& definition = resource
                ? ResourceDefinitionJson()
                : ImageDefinitionJson(mode, decimal_places);
            return a0::m2::ValidateStitchMetricResultJson(definition, json);
        },
        results,
        "result");

    const std::vector<std::pair<std::string, bool>> statistical = {
        {StatisticalResultJson(), true},
        {ReplaceOnce(ReplaceOnce(StatisticalResultJson(), "\"populationCount\":100", "\"populationCount\":9007199254740991"),
             "\"validResultCount\":98", "\"validResultCount\":9007199254740991"), true},
        {ReplaceOnce(ReplaceOnce(StatisticalResultJson(), "\"populationCount\":100", "\"populationCount\":9007199254740992"),
             "\"validResultCount\":98", "\"validResultCount\":9007199254740993"), false},
        {ReplaceOnce(StatisticalResultJson(), "statistical-result.v1", "statistical-result.v2"), false},
        {ReplaceOnce(StatisticalResultJson(), "\"validResultCount\":98", "\"validResultCount\":-1"), false},
        {ReplaceOnce(StatisticalResultJson(), "\"order\":\"AggregateThenRound\"", "\"order\":\"RoundThenAggregate\""), false},
        {ReplaceOnce(StatisticalResultJson(), "\"failureCode\":null", "\"failureCode\":\"arbitrary-token\""), false}};
    CheckSchemaRuntimeMatrix(
        statistical_schema,
        [](const std::string& json) {
            return a0::m2::ValidateStitchStatisticalResultJson(ImageDefinitionJson(), json);
        },
        statistical,
        "statistical result");

    const std::vector<std::pair<std::string, bool>> statistical_exact_safe = {
        {StatisticalResultJson("549755813888", "549755813888", 0,
             "-549755813888", "549755813888"), true},
        {StatisticalResultJson("-549755813888", "-549755813888", 0,
             "-549755813888", "549755813888"), true},
        {StatisticalResultJson("549755813889", "549755813889", 0,
             "-549755813889", "549755813889"), false}};
    CheckSchemaRuntimeMatrix(
        statistical_schema,
        [](const std::string& json) {
            return a0::m2::ValidateStitchStatisticalResultJson(
                ImageDefinitionJson("HalfToEven", 0), json);
        },
        statistical_exact_safe,
        "statistical exact-safe range");
#else
    Check(false, "A0_SOURCE_ROOT must identify checked-in schemas");
#endif
}

void TestDefinitionDomainUnitsAlgorithmsAndPolicies() {
    Check(a0::m2::ValidateStitchMetricDefinitionJson(ImageDefinitionJson()).valid,
        "complete image metric definition must validate");
    Check(a0::m2::ValidateStitchMetricDefinitionJson(ResourceDefinitionJson()).valid,
        "resource metric must use resource unit without dummy image contracts");
    for (const auto unit : {"seconds", "milliseconds", "bytes", "MiB"}) {
        Check(a0::m2::ValidateStitchMetricDefinitionJson(
                  ReplaceOnce(ResourceDefinitionJson(), "\"unit\":\"milliseconds\"",
                      std::string("\"unit\":\"") + unit + "\"")).valid,
            std::string("resource metric unit must validate: ") + unit);
    }
    Check(a0::m2::ValidateStitchMetricDefinitionJson(MedianResourceDefinitionJson()).valid,
        "median aggregation must fix the even-sample rule to MeanOfMiddlePair");
    for (const auto& invalid : {
             ReplaceOnce(ImageDefinitionJson(), "\"definitionVersion\":1", "\"definitionVersion\":\"1\""),
             ReplaceOnce(ImageDefinitionJson(), "\"metricId\":", "\"unexpected\":true,\"metricId\":"),
             ReplaceOnce(ImageDefinitionJson(), "\"level\":0.95", "\"level\":1.0"),
             ReplaceOnce(ImageDefinitionJson(), "\"decimalPlaces\":2", "\"decimalPlaces\":10"),
             ReplaceOnce(ImageDefinitionJson(), "\"percentile\":95", "\"percentile\":101"),
             ReplaceOnce(ImageDefinitionJson(), "\"percentileInterpolation\":\"LinearR7\"", "\"percentileInterpolation\":null"),
             ReplaceOnce(ImageDefinitionJson(), "\"includedValue\":1", "\"includedValue\":1.00000000000000001"),
             ReplaceOnce(ResourceDefinitionJson(), "\"algorithm\":\"PairwiseArithmeticMean\"", "\"algorithm\":\"SortedOrderStatistic\""),
             ReplaceOnce(MedianResourceDefinitionJson(), "\"medianEvenRule\":\"MeanOfMiddlePair\"", "\"medianEvenRule\":null"),
             ReplaceOnce(ResourceDefinitionJson(), "\"unit\":\"milliseconds\"", "\"unit\":\"pixels\""),
             ReplaceOnce(ResourceDefinitionJson(), "\"NotApplicable\":[\"not-applicable-for-metric-domain\"]", "\"NotApplicable\":[]")}) {
        CheckRejected(a0::m2::ValidateStitchMetricDefinitionJson(invalid),
            "range, algorithm, unit-domain, and outcome policy mismatches must reject");
    }
    CheckRejected(a0::m2::ValidateStitchMetricDefinitionJson("{\"value\":NaN}"),
        "NaN must be rejected as invalid JSON");
    CheckRejected(a0::m2::ValidateStitchMetricDefinitionJson("{\"value\":1e9999}"),
        "infinite numeric input must be rejected");
    const auto generic_underflow = ReplaceOnce(
        ImageDefinitionJson(), "\"level\":0.95", "\"level\":1e-400");
    CheckRejected(a0::m2::ValidateStitchMetricDefinitionJson(generic_underflow),
        "generic numeric underflow must not be silently treated as zero");
#ifdef A0_SOURCE_ROOT
    Check(!CheckedInSchemaAccepts(
              std::filesystem::path(A0_SOURCE_ROOT) / "docs" / "schemas" /
                  "stitch-metric-definition.schema.json", generic_underflow),
        "schema and runtime must both reject generic underflow fields");
#endif
}

void TestIndependentRoundingOracleAndOutcomeBinding() {
    const auto half_even = ImageDefinitionJson("HalfToEven");
    const auto half_away = ImageDefinitionJson("HalfAwayFromZero");
    const auto toward_zero = ImageDefinitionJson("TowardZero");
    Check(a0::m2::ValidateStitchMetricResultJson(
              half_even, SuccessResultJson("Image", "seam-registration-error", "HalfToEven", "1.245", "1.24")).valid,
        "independent oracle: HalfToEven 1.245 must round to 1.24");
    Check(a0::m2::ValidateStitchMetricResultJson(
              half_away, SuccessResultJson("Image", "seam-registration-error", "HalfAwayFromZero", "1.245", "1.25")).valid,
        "independent oracle: HalfAwayFromZero 1.245 must round to 1.25");
    Check(a0::m2::ValidateStitchMetricResultJson(
              toward_zero, SuccessResultJson("Image", "seam-registration-error", "TowardZero", "1.239", "1.23")).valid,
        "independent oracle: TowardZero 1.239 must round to 1.23");
    CheckRejected(a0::m2::ValidateStitchMetricResultJson(
        half_even, SuccessResultJson("Image", "seam-registration-error", "HalfToEven", "1.245", "1.25")),
        "HalfToEven tie must reject a HalfAwayFromZero result");
    CheckRejected(a0::m2::ValidateStitchMetricResultJson(
        half_away, SuccessResultJson("Image", "seam-registration-error", "HalfAwayFromZero", "1.245", "1.24")),
        "HalfAwayFromZero tie must reject a HalfToEven result");
    for (const auto& invalid : {
             ReplaceOnce(SuccessResultJson(), "\"sampleCount\":128", "\"sampleCount\":\"128\""),
             ReplaceOnce(SuccessResultJson(), "\"outcome\":", "\"extra\":0,\"outcome\":"),
             ReplaceOnce(SuccessResultJson(), "\"lower\":1.10,\"upper\":1.40", "\"lower\":1.50,\"upper\":1.40"),
             ReplaceOnce(SuccessResultJson(), "\"value\":0.05", "\"value\":-0.01"),
             ReplaceOnce(SuccessResultJson(), "\"rawValue\":1.245", "\"rawValue\":NaN"),
             ReplaceOnce(SuccessResultJson(), "\"rawValue\":1.245", "\"rawValue\":1e9999")}) {
        CheckRejected(a0::m2::ValidateStitchMetricResultJson(half_even, invalid),
            "result type, extra field, range, NaN, and infinite inputs must reject");
    }

    Check(a0::m2::ValidateStitchMetricResultJson(
              half_even,
              NonSuccessResultJson("Image", "seam-registration-error", "NoResult",
                  "insufficient-valid-samples")).valid,
        "definition-authorized NoResult code must validate with zero samples");
    Check(a0::m2::ValidateStitchMetricResultJson(
              ResourceDefinitionJson(),
              NonSuccessResultJson("Resource", "capture-duration", "NotApplicable",
                  "not-applicable-for-metric-domain", "HalfAwayFromZero")).valid,
        "non-image metric must have explicit NotApplicable semantics");
    CheckRejected(a0::m2::ValidateStitchMetricResultJson(
        half_even,
        NonSuccessResultJson("Image", "seam-registration-error", "NoResult", "zero-valid-results")),
        "globally known but definition-unauthorized failure code must reject");
    CheckRejected(a0::m2::ValidateStitchMetricResultJson(
        half_even,
        NonSuccessResultJson("Image", "different-metric", "NoResult", "insufficient-valid-samples")),
        "metric/version binding must reject a result for another definition");
}

void TestExactSafeRoundingAndExactValueMatching() {
#ifdef A0_SOURCE_ROOT
    const auto result_schema = std::filesystem::path(A0_SOURCE_ROOT) / "docs" / "schemas" /
        "stitch-metric-result.schema.json";
    const std::string lower = "-1000";
    const std::string upper = "1000";

    struct RoundingFixture final {
        std::string mode;
        std::string raw;
        std::string rounded;
    };
    const std::vector<RoundingFixture> fixtures = {
        {"HalfToEven", "123.5", "124"},
        {"HalfToEven", "124.5", "124"},
        {"HalfToEven", "123.49999999999999", "123"},
        {"HalfToEven", "123.50000000000001", "124"},
        {"HalfToEven", "-123.5", "-124"},
        {"HalfToEven", "-123.49999999999999", "-123"},
        {"HalfToEven", "-123.50000000000001", "-124"},
        {"HalfAwayFromZero", "123.5", "124"},
        {"HalfAwayFromZero", "-123.5", "-124"},
        {"TowardZero", "123.5", "123"},
        {"TowardZero", "-123.5", "-123"}};
    for (const auto& fixture : fixtures) {
        const auto result = SuccessResultJson("Image", "seam-registration-error", fixture.mode,
            fixture.raw, fixture.rounded, 0, lower, upper);
        Check(CheckedInSchemaAccepts(result_schema, result),
            "schema must accept a finite exact-safe tie or adjacent representable value");
        const auto definition = ImageDefinitionJson(fixture.mode, 0);
        Check(a0::m2::ValidateStitchMetricResultJson(
                  definition, result).valid,
            "runtime must classify half ties and their adjacent values exactly");
    }

    const std::vector<RoundingFixture> decimal_places_nine_fixtures = {
        {"HalfToEven", "1.2345678905", "1.234567890"},
        {"HalfToEven", "1.2345678904999999", "1.234567890"},
        {"HalfToEven", "1.2345678905000001", "1.234567891"},
        {"HalfAwayFromZero", "1.2345678905", "1.234567891"},
        {"HalfAwayFromZero", "1.2345678904999999", "1.234567890"},
        {"TowardZero", "1.2345678905", "1.234567890"},
        {"HalfToEven", "-1.2345678905", "-1.234567890"},
        {"HalfAwayFromZero", "-1.2345678905", "-1.234567891"},
        {"TowardZero", "-1.2345678905", "-1.234567890"}};
    for (const auto& fixture : decimal_places_nine_fixtures) {
        const auto result = SuccessResultJson("Image", "seam-registration-error", fixture.mode,
            fixture.raw, fixture.rounded, 9, "-2", "2");
        Check(CheckedInSchemaAccepts(result_schema, result),
            "schema must accept exponent-free dp9 tie and adjacent values");
        Check(a0::m2::ValidateStitchMetricResultJson(
                  ImageDefinitionJson(fixture.mode, 9), result).valid,
            "runtime must classify dp9 ties and adjacent values in every rounding mode");
    }

    const std::vector<std::pair<int, std::string>> exact_safe_boundaries = {
        {0, "549755813888"}, {1, "54975581388.8"}, {2, "5497558138.88"},
        {3, "549755813.888"}, {4, "54975581.3888"}, {5, "5497558.13888"},
        {6, "549755.813888"}, {7, "54975.5813888"}, {8, "5497.55813888"},
        {9, "549.755813888"}};
    for (const auto& [decimal_places, boundary] : exact_safe_boundaries) {
        const auto result = SuccessResultJson("Image", "seam-registration-error", "TowardZero",
            boundary, boundary, decimal_places, "-" + boundary, boundary);
        Check(CheckedInSchemaAccepts(result_schema, result),
            "schema must accept every inclusive decimalPlaces-specific exact-safe boundary");
        Check(a0::m2::ValidateStitchMetricResultJson(
                  ImageDefinitionJson("TowardZero", decimal_places), result).valid,
            "runtime must accept every inclusive decimalPlaces-specific exact-safe boundary");
    }

    const auto decimal_places_nine = SuccessResultJson(
        "Image", "seam-registration-error", "TowardZero",
        "1.234567890", "1.234567890", 9, lower, upper);
    Check(CheckedInSchemaAccepts(result_schema, decimal_places_nine),
        "schema must accept a value at decimalPlaces 9 inside the exact-safe range");
    Check(a0::m2::ValidateStitchMetricResultJson(
              ImageDefinitionJson("TowardZero", 9), decimal_places_nine).valid,
        "runtime must apply the exact-safe scaled rule at decimalPlaces 9");

    const auto one_quantum_mismatch = SuccessResultJson(
        "Image", "seam-registration-error", "TowardZero",
        "500000000000", "500000000001", 0,
        "-549755813888", "549755813888");
    // JSON Schema can state the safe numeric range but cannot express the
    // arithmetic equality between rawValue and value. Runtime must close it.
    Check(CheckedInSchemaAccepts(result_schema, one_quantum_mismatch),
        "schema may accept an in-range value before runtime rounding binding");
    const auto mismatch_validation = a0::m2::ValidateStitchMetricResultJson(
        ImageDefinitionJson("TowardZero", 0), one_quantum_mismatch);
    CheckRejected(mismatch_validation,
        "runtime must reject a one-quantum rounded-value mismatch");
    Check(mismatch_validation.failure_code == "metric_value_invalid",
        "rounded-value mismatch must retain metric_value_invalid failure semantics");

    const auto nine_place_mismatch = SuccessResultJson(
        "Image", "seam-registration-error", "TowardZero",
        "1.234567890", "1.234567891", 9, lower, upper);
    Check(CheckedInSchemaAccepts(result_schema, nine_place_mismatch),
        "schema must not replace runtime arithmetic binding at decimalPlaces 9");
    CheckRejected(a0::m2::ValidateStitchMetricResultJson(
        ImageDefinitionJson("TowardZero", 9), nine_place_mismatch),
        "runtime must reject a one-quantum mismatch at decimalPlaces 9");
#else
    Check(false, "A0_SOURCE_ROOT must identify checked-in schemas");
#endif
}

void TestStatisticalResultBindingAndRounding() {
    Check(a0::m2::ValidateStitchStatisticalResultJson(
              ImageDefinitionJson(), StatisticalResultJson()).valid,
        "statistical result must bind to definition aggregation and use raw-then-round");
    Check(a0::m2::ValidateStitchStatisticalResultJson(
              ImageDefinitionJson(), StatisticalNoResultJson()).valid,
        "ZeroValidSamples rule must permit NoResult only with zero valid results");
    CheckRejected(a0::m2::ValidateStitchStatisticalResultJson(
        ImageDefinitionJson(),
        ReplaceOnce(StatisticalNoResultJson(), "\"validResultCount\":0", "\"validResultCount\":1")),
        "NoResult must reject a nonzero valid-result count");
    for (const auto& invalid : {
             ReplaceOnce(StatisticalResultJson(), "\"populationCount\":100,", ""),
             ReplaceOnce(StatisticalResultJson(), "\"validResultCount\":98", "\"validResultCount\":101"),
             ReplaceOnce(StatisticalResultJson(), "\"percentile\":95", "\"percentile\":-1"),
             ReplaceOnce(StatisticalResultJson(), "\"algorithm\":\"SortedOrderStatistic\"", "\"algorithm\":\"SortedMiddle\""),
             ReplaceOnce(StatisticalResultJson(), "\"value\":1.30", "\"value\":1.31"),
             ReplaceOnce(StatisticalResultJson(), "\"failureCode\":null", "\"failureCode\":false")}) {
        CheckRejected(a0::m2::ValidateStitchStatisticalResultJson(ImageDefinitionJson(), invalid),
            "statistical shape, aggregation, ranges, rounding, and types must reject");
    }
}

} // namespace

int main() {
    TestCheckedInSchemasDirectlyValidateFixtures();
    TestDefinitionDomainUnitsAlgorithmsAndPolicies();
    TestIndependentRoundingOracleAndOutcomeBinding();
    TestExactSafeRoundingAndExactValueMatching();
    TestStatisticalResultBindingAndRounding();
    if (failures != 0) {
        std::cerr << failures << " stitch metric contract test(s) failed\n";
        return 1;
    }
    std::cout << "All stitch metric contract tests passed\n";
    return 0;
}
