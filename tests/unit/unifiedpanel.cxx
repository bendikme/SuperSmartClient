/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtest/gtest.h>
#include <rfb/SecurityClient.h>
#include "parameters.h"

class UnifiedPanel : public testing::Test {
protected:
  void SetUp() override
  {
    unifiedPanel.setParam(true);
    unifiedSecurity.setParam("Certificate");
    rfb::SecurityClient::secTypes.setParam("None,VncAuth");
    shared.setParam(false);
    remoteResize.setParam(true);
    desktopSize.setParam("1920x1080");
    sendClipboard.setParam(true);
    acceptClipboard.setParam(true);
    viewOnly.setParam(false);
  }
};

TEST_F(UnifiedPanel, StandardProfilePreservesUserSettings)
{
  unifiedPanel.setParam(false);
  applyUnifiedPanelProfile();
  rfb::SecurityClient security;
  EXPECT_TRUE(security.IsSupported(rfb::secTypeVncAuth));
  EXPECT_TRUE(security.IsSupported(rfb::secTypeNone));
  EXPECT_FALSE(shared);
  EXPECT_TRUE(remoteResize);
  EXPECT_STREQ(desktopSize, "1920x1080");
  EXPECT_TRUE(sendClipboard);
  EXPECT_TRUE(acceptClipboard);
}

#ifdef HAVE_GNUTLS
TEST_F(UnifiedPanel, CertificateModeRequiresEncryptedPasswordAuthentication)
{
  applyUnifiedPanelProfile();
  rfb::SecurityClient security;
  EXPECT_EQ(security.GetEnabledSecTypes(),
            std::list<uint8_t>({rfb::secTypeVeNCrypt}));
  EXPECT_EQ(security.GetEnabledExtSecTypes(),
            std::list<uint32_t>({rfb::secTypeX509Vnc}));
  EXPECT_FALSE(security.IsSupported(rfb::secTypeVncAuth));
  EXPECT_FALSE(security.IsSupported(rfb::secTypeNone));
  EXPECT_FALSE(security.IsSupported(rfb::secTypeTLSVnc));
  EXPECT_FALSE(security.IsSupported(rfb::secTypeX509None));
}

TEST_F(UnifiedPanel, AnonymousTLSRequiresExplicitSelection)
{
  unifiedSecurity.setParam("AnonymousTLS");
  applyUnifiedPanelProfile();
  rfb::SecurityClient security;
  EXPECT_EQ(security.GetEnabledExtSecTypes(),
            std::list<uint32_t>({rfb::secTypeTLSVnc}));
  EXPECT_FALSE(security.IsSupported(rfb::secTypeVncAuth));
  EXPECT_FALSE(security.IsSupported(rfb::secTypeTLSNone));
}

TEST_F(UnifiedPanel, PreventsResizeClipboardAndExclusiveConnections)
{
  applyUnifiedPanelProfile();
  EXPECT_TRUE(shared);
  EXPECT_FALSE(remoteResize);
  EXPECT_STREQ(desktopSize, "");
  EXPECT_FALSE(sendClipboard);
  EXPECT_FALSE(acceptClipboard);
#if !defined(WIN32) && !defined(__APPLE__)
  EXPECT_FALSE(sendPrimary);
#endif
}

TEST_F(UnifiedPanel, TLSModesPreserveMonitorAndControlChoices)
{
  for (const char* mode : {"Certificate", "AnonymousTLS"}) {
    unifiedSecurity.setParam(mode);
    viewOnly.setParam(true);
    applyUnifiedPanelProfile();
    EXPECT_TRUE(viewOnly);
    viewOnly.setParam(false);
    applyUnifiedPanelProfile();
    EXPECT_FALSE(viewOnly);
  }
}

TEST_F(UnifiedPanel, ReappliesPolicyAfterConflictingSettings)
{
  applyUnifiedPanelProfile();
  rfb::SecurityClient::secTypes.setParam("None,VncAuth,TLSNone");
  shared.setParam(false);
  remoteResize.setParam(true);
  sendClipboard.setParam(true);
  applyUnifiedPanelProfile();
  rfb::SecurityClient security;
  EXPECT_EQ(security.GetEnabledExtSecTypes(),
            std::list<uint32_t>({rfb::secTypeX509Vnc}));
  EXPECT_TRUE(shared);
  EXPECT_FALSE(remoteResize);
  EXPECT_FALSE(sendClipboard);
}
#else
TEST_F(UnifiedPanel, RefusesBuildsWithoutTLS)
{
  EXPECT_THROW(applyUnifiedPanelProfile(), std::runtime_error);
}
#endif
