/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rkp_factory_extraction_lib.h"

#include <aidl/android/hardware/security/keymint/IRemotelyProvisionedComponent.h>
#include <android/binder_manager.h>
#include <cppbor.h>
#include <keymaster/cppcose/cppcose.h>
#include <openssl/base64.h>
#include <remote_prov/remote_prov_utils.h>
#include <sys/random.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppbor_parse.h"

using aidl::android::hardware::security::keymint::DeviceInfo;
using aidl::android::hardware::security::keymint::IRemotelyProvisionedComponent;
using aidl::android::hardware::security::keymint::MacedPublicKey;
using aidl::android::hardware::security::keymint::ProtectedData;
using aidl::android::hardware::security::keymint::RpcHardwareInfo;
using aidl::android::hardware::security::keymint::remote_prov::getProdEekChain;
using aidl::android::hardware::security::keymint::remote_prov::jsonEncodeCsrWithBuild;

using namespace cppbor;
using namespace cppcose;

std::string toBase64(const std::vector<uint8_t>& buffer) {
    size_t base64Length;
    int rc = EVP_EncodedLength(&base64Length, buffer.size());
    if (!rc) {
        std::cerr << "Error getting base64 length. Size overflow?" << std::endl;
        exit(-1);
    }

    std::string base64(base64Length, ' ');
    rc = EVP_EncodeBlock(reinterpret_cast<uint8_t*>(base64.data()), buffer.data(), buffer.size());
    ++rc;  // Account for NUL, which BoringSSL does not for some reason.
    if (rc != base64Length) {
        std::cerr << "Error writing base64. Expected " << base64Length
                  << " bytes to be written, but " << rc << " bytes were actually written."
                  << std::endl;
        exit(-1);
    }

    // BoringSSL automatically adds a NUL -- remove it from the string data
    base64.pop_back();

    return base64;
}

std::vector<uint8_t> generateChallenge() {
    std::vector<uint8_t> challenge(kChallengeSize);

    ssize_t bytesRemaining = static_cast<ssize_t>(challenge.size());
    uint8_t* writePtr = challenge.data();
    while (bytesRemaining > 0) {
        int bytesRead = getrandom(writePtr, bytesRemaining, /*flags=*/0);
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                std::cerr << errno << ": " << strerror(errno) << std::endl;
                exit(-1);
            }
        }
        bytesRemaining -= bytesRead;
        writePtr += bytesRead;
    }

    return challenge;
}

CsrResult composeCertificateRequest(const ProtectedData& protectedData,
                                    const DeviceInfo& verifiedDeviceInfo,
                                    const std::vector<uint8_t>& challenge,
                                    const std::vector<uint8_t>& keysToSignMac) {
    Array macedKeysToSign = Array()
                                .add(Map().add(1, 5).encode())  // alg: hmac-sha256
                                .add(Map())                     // empty unprotected headers
                                .add(Null())                    // nil for the payload
                                .add(keysToSignMac);            // MAC as returned from the HAL

    auto [parsedVerifiedDeviceInfo, ignore1, errMsg] = parse(verifiedDeviceInfo.deviceInfo);
    if (!parsedVerifiedDeviceInfo) {
        std::cerr << "Error parsing device info: '" << errMsg << "'" << std::endl;
        return {nullptr, errMsg};
    }

    auto [parsedProtectedData, ignore2, errMsg2] = parse(protectedData.protectedData);
    if (!parsedProtectedData) {
        std::cerr << "Error parsing protected data: '" << errMsg2 << "'" << std::endl;
        return {nullptr, errMsg};
    }

    Array deviceInfo = Array().add(std::move(parsedVerifiedDeviceInfo)).add(Map());

    auto certificateRequest = std::make_unique<Array>();
    (*certificateRequest)
        .add(std::move(deviceInfo))
        .add(challenge)
        .add(std::move(parsedProtectedData))
        .add(std::move(macedKeysToSign));
    return {std::move(certificateRequest), std::nullopt};
}

CsrResult getCsr(std::string_view componentName, IRemotelyProvisionedComponent* irpc) {
    std::vector<uint8_t> keysToSignMac;
    std::vector<MacedPublicKey> emptyKeys;
    DeviceInfo verifiedDeviceInfo;
    ProtectedData protectedData;
    RpcHardwareInfo hwInfo;
    ::ndk::ScopedAStatus status = irpc->getHardwareInfo(&hwInfo);
    if (!status.isOk()) {
        std::cerr << "Failed to get hardware info for '" << componentName
                  << "'. Error code: " << status.getServiceSpecificError() << "." << std::endl;
        exit(-1);
    }

    const std::vector<uint8_t> eek = getProdEekChain(hwInfo.supportedEekCurve);
    const std::vector<uint8_t> challenge = generateChallenge();
    status = irpc->generateCertificateRequest(
        /*test_mode=*/false, emptyKeys, eek, challenge, &verifiedDeviceInfo, &protectedData,
        &keysToSignMac);
    if (!status.isOk()) {
        std::cerr << "Bundle extraction failed for '" << componentName
                  << "'. Error code: " << status.getServiceSpecificError() << "." << std::endl;
        exit(-1);
    }
    return composeCertificateRequest(protectedData, verifiedDeviceInfo, challenge, keysToSignMac);
}