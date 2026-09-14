/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "DashboardModel.h"
#include <stdexcept>

#ifdef WIN32
#include <windows.h>
#include <wincrypt.h>
#elif defined(HAVE_LIBSECRET)
#include <libsecret/secret.h>
static const SecretSchema passwordSchema = {
  "org.supersmartclient.Panel", SECRET_SCHEMA_NONE,
  {{"id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
  0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
#endif

namespace dashboard {

bool passwordStorageAvailable()
{
#if defined(WIN32) || defined(HAVE_LIBSECRET)
  return true;
#else
  return false;
#endif
}

std::string protectPassword(const std::string& id, const std::string& password)
{
  if (password.empty()) return "";
#ifdef WIN32
  std::string purpose = "SuperSmartClient/dashboard/" + id;
  DATA_BLOB input{static_cast<DWORD>(password.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(password.data()))};
  DATA_BLOB entropy{static_cast<DWORD>(purpose.size()),
                    reinterpret_cast<BYTE*>(&purpose[0])}, output{};
  if (!CryptProtectData(&input, L"SuperSmartClient panel password", &entropy,
                       nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
    throw std::runtime_error("Windows could not protect the saved password");
  std::string result = "dpapi:" + hexEncode(std::string(
    reinterpret_cast<char*>(output.pbData), output.cbData));
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return result;
#elif defined(HAVE_LIBSECRET)
  GError* error = nullptr;
  bool stored = secret_password_store_sync(&passwordSchema, SECRET_COLLECTION_DEFAULT,
    "SuperSmartClient panel", password.c_str(), nullptr, &error, "id", id.c_str(), nullptr);
  if (!stored) {
    std::string message = error ? error->message : "Desktop keyring is unavailable";
    if (error) g_error_free(error);
    throw std::runtime_error("Cannot save password in the desktop keyring: " + message);
  }
  return "keyring:" + id;
#else
  (void)id;
  throw std::runtime_error("Password storage requires Windows or a build with libsecret");
#endif
}

std::string unprotectPassword(const std::string& id, const std::string& credential)
{
  if (credential.empty()) return "";
#ifdef WIN32
  if (credential.compare(0, 6, "dpapi:") != 0)
    throw std::runtime_error("Saved password belongs to another operating system");
  std::string encrypted = hexDecode(credential.substr(6));
  if (encrypted.empty()) throw std::runtime_error("Saved password is empty or damaged");
  std::string purpose = "SuperSmartClient/dashboard/" + id;
  DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                  reinterpret_cast<BYTE*>(&encrypted[0])};
  DATA_BLOB entropy{static_cast<DWORD>(purpose.size()),
                    reinterpret_cast<BYTE*>(&purpose[0])}, output{};
  if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &output))
    throw std::runtime_error("Saved password is unavailable for this Windows account. Edit the connection.");
  std::string password(reinterpret_cast<char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return password;
#elif defined(HAVE_LIBSECRET)
  if (credential != "keyring:" + id)
    throw std::runtime_error("Saved password belongs to another operating system");
  GError* error = nullptr;
  gchar* value = secret_password_lookup_sync(&passwordSchema, nullptr, &error,
                                             "id", id.c_str(), nullptr);
  if (!value) {
    std::string message = error ? error->message : "Saved password was not found";
    if (error) g_error_free(error);
    throw std::runtime_error("Cannot read the saved password: " + message);
  }
  std::string password(value);
  secret_password_free(value);
  return password;
#else
  (void)id;
  throw std::runtime_error("This build cannot read saved passwords");
#endif
}

void forgetPassword(const std::string& id, const std::string& credential)
{
#if !defined(WIN32) && defined(HAVE_LIBSECRET)
  if (credential == "keyring:" + id) {
    GError* error = nullptr;
    secret_password_clear_sync(&passwordSchema, nullptr, &error, "id", id.c_str(), nullptr);
    if (error) {
      std::string message(error->message);
      g_error_free(error);
      throw std::runtime_error("Cannot remove saved password: " + message);
    }
  }
#else
  (void)id;
  (void)credential;
  // DPAPI ciphertext is removed with the connection record.
#endif
}

}
