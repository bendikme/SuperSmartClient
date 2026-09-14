/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtest/gtest.h>
#include <core/Rect.h>
#include <rdr/MemOutStream.h>
#include <rfb/CMsgWriter.h>
#include <rfb/ServerParams.h>
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

TEST_F(UnifiedPanel, EnforcesMonitorOnlyDespiteConflictingSettings)
{
  viewOnly.setParam(true);
  applyUnifiedPanelProfile();
  EXPECT_TRUE(viewOnly);
  viewOnly.setParam(false);
  applyUnifiedPanelProfile();
  EXPECT_TRUE(viewOnly);
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

TEST(UnifiedPanelInput, BlocksMouseAndKeyboardAtTheProtocolWriter)
{
  rfb::ServerParams server;
  server.setDimensions(64, 48);
  rdr::MemOutStream output;
  rfb::CMsgWriter writer(&server, &output, false);

  for (bool extended : {false, true}) {
    server.supportsQEMUKeyEvent = extended;
    server.supportsExtendedMouseButtons = extended;
    writer.writeKeyEvent('a', 0x1e, true);
    writer.writeKeyEvent('a', 0x1e, false);
    writer.writePointerEvent(core::Point(12, 24), 0); // Motion
    writer.writePointerEvent(core::Point(12, 24), 1); // Button press
    writer.writePointerEvent(core::Point(12, 24), 8); // Scroll wheel
    writer.writePointerEvent(core::Point(12, 24), 128); // Extended button
  }
  EXPECT_EQ(output.length(), 0u);

  writer.writeFramebufferUpdateRequest(core::Rect(0, 0, 64, 48), true);
  ASSERT_EQ(output.length(), 10u);
  EXPECT_EQ(output.data()[0], 3); // Display updates remain available.
}

TEST(UnifiedPanelInput, StandardWriterStillSupportsInput)
{
  rfb::ServerParams server;
  server.setDimensions(64, 48);
  rdr::MemOutStream output;
  rfb::CMsgWriter writer(&server, &output);
  writer.writeKeyEvent('a', 0, true);
  writer.writePointerEvent(core::Point(12, 24), 1);
  const uint8_t expected[] = {4, 1, 0, 0, 0, 0, 0, 'a', 5, 1, 0, 12, 0, 24};
  ASSERT_EQ(output.length(), sizeof(expected));
  EXPECT_EQ(memcmp(output.data(), expected, sizeof(expected)), 0);
}
