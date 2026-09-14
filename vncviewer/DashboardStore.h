/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SUPERSMART_DASHBOARD_STORE_H
#define SUPERSMART_DASHBOARD_STORE_H
#include "DashboardModel.h"

namespace dashboard {
struct SavedLayout { std::string id, name; Workspace workspace; };
struct Library { std::string active; std::vector<SavedLayout> layouts; };
Library newLibrary();
Library loadLibrary(const std::filesystem::path& path);
void saveLibrary(const std::filesystem::path& path, const Library& library);
size_t activeLayout(const Library& library);
void validateLayoutName(const std::string& name);
SavedLayout duplicateLayout(const SavedLayout& source, const std::string& name);
void mergeLibrary(Library& destination, const Library& imported);
// Empty export password omits all credentials. With a password, the entire
// portable database is authenticated and encrypted before any bytes reach disk.
void exportLibrary(const std::filesystem::path& path, const Library& library,
                   const std::string& password);
Library importLibrary(const std::filesystem::path& path, const std::string& password);
bool encryptedExport(const std::filesystem::path& path);
}
#endif
