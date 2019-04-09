/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2019-present Percona and/or its affiliates. All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the Server Side Public License, version 1,
    as published by MongoDB, Inc.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Server Side Public License for more details.

    You should have received a copy of the Server Side Public License
    along with this program. If not, see
    <http://www.mongodb.com/licensing/server-side-public-license>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the Server Side Public License in all respects for
    all of the code used other than as permitted herein. If you modify file(s)
    with this exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do so,
    delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then also delete
    it in the license file.
======= */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/db/encryption/encryption_vault.h"

#include <curl/curl.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/encryption/encryption_options.h"
#include "mongo/db/json.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

class CURLGuard {
    MONGO_DISALLOW_COPYING(CURLGuard);

public:
    CURLGuard() {}
    ~CURLGuard() {
        curl_global_cleanup();
    }

    void initialize() {
        CURLcode ret = curl_global_init(CURL_GLOBAL_ALL);
        if (ret != CURLE_OK) {
            throw std::runtime_error(str::stream()
                                     << "failed to initialize CURL: " << static_cast<int64_t>(ret));
        }

        curl_version_info_data* version_data = curl_version_info(CURLVERSION_NOW);
        if (!(version_data->features & CURL_VERSION_SSL)) {
            throw std::runtime_error(str::stream()
                                     << "Curl lacks SSL support, cannot continue");
        }
    }
};

class Curl_session_guard {
    MONGO_DISALLOW_COPYING(Curl_session_guard);

public:
    Curl_session_guard(CURL *curl)
        : curl(curl)
    {}
    ~Curl_session_guard()
    {
        if (curl != nullptr)
            curl_easy_cleanup(curl);
    }

private:
    CURL *curl;
};

class Curl_slist_guard {
    MONGO_DISALLOW_COPYING(Curl_slist_guard);

public:
    Curl_slist_guard(curl_slist *list)
        : list(list)
    {}
    ~Curl_slist_guard() {
        if (list != nullptr)
            curl_slist_free_all(list);
    }

private:
    curl_slist *list;
};

size_t write_response_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    *(str::stream*)userdata << std::string(ptr, nmemb);
    return nmemb;
}

CURLcode setup_curl_options(CURL *curl) {
    CURLcode curl_res = CURLE_OK;
    (curl_res = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)) != CURLE_OK ||
    (curl_res = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) != CURLE_OK ||
    (!encryptionGlobalParams.vaultServerCAFile.empty() &&
        (curl_res = curl_easy_setopt(curl, CURLOPT_CAINFO, encryptionGlobalParams.vaultServerCAFile.c_str())) != CURLE_OK
    ) ||
    (curl_res = curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL)) != CURLE_OK ||
    (curl_res = curl_easy_setopt(curl, CURLOPT_TIMEOUT, encryptionGlobalParams.vaultTimeout)) != CURLE_OK ||
    (curl_res = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, encryptionGlobalParams.vaultTimeout)) != CURLE_OK;
    return curl_res;
}

void throw_CURL_error(CURLcode curl_res, const char curl_errbuf[], const char* msg) {
    str::stream ss;
    ss << msg << "; CURL error code: " << curl_res << "; CURL error message: ";
    if (curl_errbuf[0])
        ss << curl_errbuf;
    else
        ss << curl_easy_strerror(curl_res);
    throw std::runtime_error(ss);
}

} // namespace

std::string vaultReadKey() {
    char curl_errbuf[CURL_ERROR_SIZE]{0}; // should be available until curl_easy_cleanup
    long http_code{0};
    CURLGuard guard;
    guard.initialize();

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error(str::stream()
                                 << "Cannot initialize curl session");
    }
    Curl_session_guard curl_session_guard(curl);

    CURLcode curl_res = CURLE_OK;
    str::stream response;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, std::string(str::stream() << "X-Vault-Token: " << encryptionGlobalParams.vaultToken).c_str());
    Curl_slist_guard curl_slist_guard(headers);

    if ((curl_res = setup_curl_options(curl)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response_callback)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&response))) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_URL,
                                     std::string(str::stream()
                                     << "http://" << encryptionGlobalParams.vaultServerName
                                     << ':'       << encryptionGlobalParams.vaultPort
                                     << "/v1/"    << encryptionGlobalParams.vaultSecret).c_str())) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers)) != CURLE_OK ||
        (curl_res = curl_easy_perform(curl)) != CURLE_OK ||
        (curl_res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code)) != CURLE_OK) {
        throw_CURL_error(curl_res, curl_errbuf, "Error reading key from the Vault");
    }

    // response may contain encryption key
    // log() << std::string(response);
    LOG(4) << "HTTP code (GET): " << http_code;
    if (http_code == 404) {
        // requested value does not exist - return empty string
        return {};
    }
    if (http_code / 100 != 2) {
        // not success - throw error
        throw std::runtime_error(str::stream()
                                 << "Error reading key from the Vault; HTTP code: "
                                 << http_code);
    }
    BSONObj bson = fromjson(response);
    BSONElement data1 = bson["data"];
    if (data1.eoo() || !data1.isABSONObj()) {
        throw std::runtime_error("Error parsing Vault response");
    }
    BSONElement data2 = data1.Obj()["data"];
    if (data2.eoo() || !data2.isABSONObj()) {
        throw std::runtime_error("Error parsing Vault response");
    }
    BSONElement value = data2.Obj()["value"];
    if (value.eoo() || value.type() != mongo::String) {
        throw std::runtime_error("Error parsing Vault response");
    }
    return value.String();
}

void vaultWriteKey(std::string const& key) {
    char curl_errbuf[CURL_ERROR_SIZE]{0}; // should be available until curl_easy_cleanup
    long http_code{0};
    CURLGuard guard;
    guard.initialize();

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error(str::stream()
                                 << "Cannot initialize curl session");
    }
    Curl_session_guard curl_session_guard(curl);

    CURLcode curl_res = CURLE_OK;
    str::stream response;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, std::string(str::stream() << "X-Vault-Token: " << encryptionGlobalParams.vaultToken).c_str());
    Curl_slist_guard curl_slist_guard(headers);

    std::string urlstr = std::string(str::stream()
                                     << "http://" << encryptionGlobalParams.vaultServerName
                                     << ':'       << encryptionGlobalParams.vaultPort
                                     << "/v1/"    << encryptionGlobalParams.vaultSecret);
    std::string postdata = std::string(str::stream()
                                       << "{\"data\": "
                                       << "{\"value\": \"" << key
                                       << "\"}}");
    if ((curl_res = setup_curl_options(curl)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response_callback)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&response))) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_URL, urlstr.c_str())) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers)) != CURLE_OK ||
        (curl_res = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata.c_str())) != CURLE_OK ||
        (curl_res = curl_easy_perform(curl)) != CURLE_OK ||
        (curl_res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code)) != CURLE_OK) {
        throw_CURL_error(curl_res, curl_errbuf, "Error writing key to the Vault");
    }

    // log() << std::string(response);
    LOG(4) << "HTTP code (POST): " << http_code;
    if (http_code / 100 != 2) {
        // not success - throw error
        throw std::runtime_error(str::stream()
                                 << "Error writing key to the Vault; HTTP code: "
                                 << http_code);
    }
}

}  // namespace mongo
