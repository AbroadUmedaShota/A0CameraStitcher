// Isolated stage-one hardware worker, NOT the two-camera controller.
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/single_worker_preview.hpp"
#include "nikon_sdk_runtime_path.hpp"
#include <Windows.h>
#include <chrono>
#include <iostream>
#include <string_view>

using namespace a0::phase0;
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "describe") {
        std::cout << "{\"schema\":\"a0.single-worker-preview.v1\",\"operations\":[\"describe\",\"preview-single\"],"
                     "\"requiredFlag\":\"--confirm-one-physical-camera\",\"frames\":3,\"capture\":false,"
                     "\"settings\":false,\"wpd\":false,\"dualEnabled\":false,\"bindingProof\":false,\"sdkAvailable\":"
                  << (NikonSdkTransport::LicensedAdapterAvailable() ? "true" : "false") << "}\n";
        return 0;
    }
    if (argc != 3 || std::string_view(argv[1]) != "preview-single" ||
        std::string_view(argv[2]) != "--confirm-one-physical-camera") {
        std::cout << "{\"status\":\"rejected\",\"error\":\"explicit_single_camera_profile_required\"}\n";
        return 2;
    }
    // These checks load no SDK and open no camera.
    if (!NikonSdkTransport::LicensedAdapterAvailable() || !detail::ResolveNikonSdkModuleFromEnvironment()) {
        std::cout << "{\"status\":\"unavailable\",\"error\":\"licensed_sdk_required\"}\n";
        return 3;
    }
    try {
        // Single-worker commissioning only: this one process owns the existing
        // production lease throughout SDK use and quarantine. No delegation and
        // no second worker are permitted. A future two-worker owner is separate.
        HardwareProcessLease lease;
        if (lease.RecoveredAbandonedOwner()) {
            std::cout << "{\"status\":\"quarantined\",\"error\":\"abandoned_camera_owner\"}\n" << std::flush;
            std::cerr << "Previous owner ended without confirmed cleanup. Lease retained; human recovery required.\n";
            Sleep(INFINITE);
            return 4;
        }
        NikonSdkTransport transport;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        const auto result = RunSingleWorkerPreview(transport,
            [](std::string_view stage) {
                std::cout << "{\"mode\":\"hardware\",\"status\":\"running\",\"stage\":\"" << stage << "\"}\n" << std::flush;
            },
            [&] {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw TransportError("worker_deadline", "no new SDK operation after preview deadline");
            });
        std::cout << "{\"schema\":\"a0.single-worker-preview.v1\",\"mode\":\"hardware\",\"status\":\""
                  << (result.Passed() ? "passed" : (result.SafeToRelease() ? "failed" : "quarantined"))
                  << "\",\"bindingProof\":false,\"frames\":" << result.frames << ",\"bytes\":" << result.bytes
                  << ",\"stopConfirmed\":" << (result.stopped ? "true" : "false")
                  << ",\"closeConfirmed\":" << (result.closed ? "true" : "false")
                  << ",\"error\":\"" << result.error << "\"}\n" << std::flush;
        if (!result.SafeToRelease()) {
            // Intentional quarantine, not success/automatic recovery. Keep the
            // same thread alive (Windows mutex ownership is thread-affine).
            // Human must make the camera safe before manually ending this process.
            std::cerr << "Camera stop/close unconfirmed: lease retained. No retry. Human recovery required.\n";
            Sleep(INFINITE);
        }
        return result.Passed() ? 0 : 5;
    } catch (const TransportError& e) {
        std::cout << "{\"status\":\"rejected\",\"error\":\"" << e.Category() << "\"}\n";
        return 6;
    } catch (...) { std::cout << "{\"status\":\"failed\",\"error\":\"unexpected_error\"}\n"; return 7; }
}
