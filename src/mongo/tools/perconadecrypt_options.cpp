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

#include "mongo/tools/perconadecrypt_options.h"

#include <iostream>

#include "mongo/db/encryption/encryption_options.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

PerconaDecryptGlobalParams perconaDecryptGlobalParams;

Status addPerconaDecryptOptions(moe::OptionSection* options) {
    options->addOptionChaining("help", "help", moe::Switch, "show this usage information");

    options->addOptionChaining("inputPath", "inputPath", moe::String, "encrypted file to decrypt");

    options->addOptionChaining("outputPath", "outputPath", moe::String, "where to store decrypted result");

    options->addOptionChaining("encryptionKeyFile", "encryptionKeyFile", moe::String, "the path to encryption key file");

    options->addOptionChaining("encryptionCipherMode", "encryptionCipherMode", moe::String, "the cipher mode to use for decryption (AES256-CBC/AES256-GCM)")
        .format("(:?AES256-CBC)|(:?AES256-GCM)", "(AES256-CBC/AES256-GCM)");

    options->addOptionChaining(
            "vaultServerName",
            "vaultServerName",
            moe::String,
            "hostname or IP address of the Vault server")
        .requires("vaultPort")
        .requires("vaultTokenFile")
        .requires("vaultSecret")
        .incompatibleWith("encryptionKeyFile");

    options->addOptionChaining(
            "vaultPort",
            "vaultPort",
            moe::Int,
            "port name the Vault server is listening on")
        .requires("vaultServerName")
        .validRange(0, 65535);

    options->addOptionChaining(
            "vaultTokenFile",
            "vaultTokenFile",
            moe::String,
            "the path to file with Vault server's access token")
        .requires("vaultServerName");

    options->addOptionChaining(
            "vaultSecret",
            "vaultSecret",
            moe::String,
            "the name of the Vault secret where the master key is stored")
        .requires("vaultServerName");

    options->addOptionChaining(
            "vaultServerCAFile",
            "vaultServerCAFile",
            moe::String,
            "CA certificate that was used to sign Vault’s certificates "
            "- should be used when the Vault’s CA certificate is not trusted "
            "by the machine that is going to connect to the Vault server")
        .requires("vaultServerName");

    options->addOptionChaining(
            "vaultDisableTLSForTesting",
            "vaultDisableTLSForTesting",
            moe::Switch,
            "disable using TLS for Vault server connections "
            "- is suitable for connecting Vault server in -dev mode or "
            "Vault server with TLS disabled. Should not be used in production")
        .requires("vaultServerName");

    return Status::OK();
}

void printPerconaDecryptHelp(std::ostream* out) {
    *out << "Usage:" << std::endl
         << "    perconadecrypt [options] --inputPath <src> --outputPath <dest> --encryptionKeyFile <key path>" << std::endl
         << "    perconadecrypt [options] --inputPath <src> --outputPath <dest> --vaultServerName <server name> [other Vault options]" << std::endl
         << "    perconadecrypt --help" << std::endl;
    *out << moe::startupOptions.helpString();
    *out << std::flush;
}

bool handlePreValidationPerconaDecryptOptions(const moe::Environment& params) {
    if (params.count("help")) {
        printPerconaDecryptHelp(&std::cout);
        return false;
    }
    return true;
}

Status storePerconaDecryptOptions(const moe::Environment& params,
                               const std::vector<std::string>& args) {
    if (!params.count("encryptionKeyFile") && !params.count("vaultServerName")) {
        return {ErrorCodes::BadValue, "Missing required option: one of --encryptionKeyFile and --vaultServerName must be specified"};
    }

    if (params.count("encryptionKeyFile")) {
        encryptionGlobalParams.encryptionKeyFile = params["encryptionKeyFile"].as<std::string>();
    }

    if (params.count("vaultServerName")) {
        encryptionGlobalParams.vaultServerName = params["vaultServerName"].as<std::string>();
    }

    if (params.count("vaultPort")) {
        encryptionGlobalParams.vaultPort = params["vaultPort"].as<int>();
    }

    if (params.count("vaultTokenFile")) {
        encryptionGlobalParams.vaultTokenFile = params["vaultTokenFile"].as<std::string>();
    }

    if (params.count("vaultSecret")) {
        encryptionGlobalParams.vaultSecret = params["vaultSecret"].as<std::string>();
    }

    if (params.count("vaultServerCAFile")) {
        encryptionGlobalParams.vaultServerCAFile = params["vaultServerCAFile"].as<std::string>();
    }

    if (params.count("vaultDisableTLSForTesting")) {
        encryptionGlobalParams.vaultDisableTLS = params["vaultDisableTLSForTesting"].as<bool>();
    }


    if (params.count("encryptionCipherMode")) {
        encryptionGlobalParams.encryptionCipherMode = params["encryptionCipherMode"].as<std::string>();
    }

    if (!params.count("inputPath")) {
        return {ErrorCodes::BadValue, "Missing required option: --inputPath"};
    }
    perconaDecryptGlobalParams.inputPath = params["inputPath"].as<std::string>();

    if (!params.count("outputPath")) {
        return {ErrorCodes::BadValue, "Missing required option: --outputPath"};
    }
    perconaDecryptGlobalParams.outputPath = params["outputPath"].as<std::string>();

    return Status::OK();
}

}  // namespace mongo
