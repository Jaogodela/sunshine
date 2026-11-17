/**
 * @file tests/unit/test_sp_pairing.cpp
 * @brief Test Gaspar pre-provisioned client trust and PIN pairing disablement.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

// local includes
#include <src/config.h>
#include <src/file_handler.h>
#include <src/nvhttp.h>

namespace fs = std::filesystem;

namespace {
  /**
   * @brief Verify that the Gaspar configuration keys parse and retain their upstream defaults.
   */
  class SpPairingConfigTest: public BaseTest {
  protected:
    /**
     * @brief Restore the complete configuration after a parsing test.
     */
    void TearDown() override {
      config::video = original_video_;
      config::audio = original_audio_;
      config::stream = original_stream_;
      config::nvhttp = original_nvhttp_;
      config::input = original_input_;
      config::sunshine = original_sunshine_;
      config::modified_config_settings = original_modified_config_settings_;
      BaseTest::TearDown();
    }

  private:
    config::video_t original_video_ {config::video};  ///< Video configuration restored after each test.
    config::audio_t original_audio_ {config::audio};  ///< Audio configuration restored after each test.
    config::stream_t original_stream_ {config::stream};  ///< Stream configuration restored after each test.
    config::nvhttp_t original_nvhttp_ {config::nvhttp};  ///< HTTP configuration restored after each test.
    config::input_t original_input_ {config::input};  ///< Input configuration restored after each test.
    config::sunshine_t original_sunshine_ {config::sunshine};  ///< Core configuration restored after each test.
    decltype(config::modified_config_settings) original_modified_config_settings_ {config::modified_config_settings};  ///< Modified settings restored after each test.
  };

  /**
   * @brief Isolate pre-provisioned client authorization tests from the user's Sunshine state.
   */
  class SpTrustedClientTest: public BaseTest {
  protected:
    /**
     * @brief Redirect persisted state to the test build directory and clear trust options.
     */
    void SetUp() override {
      BaseTest::SetUp();

      config::nvhttp = original_nvhttp_;
      config::sunshine.flags[config::flag::FRESH_STATE] = false;

      state_file_ = fs::path {SUNSHINE_TEST_BIN_DIR} / "sp_trusted_client_state.json";
      cert_file_ = fs::path {SUNSHINE_TEST_BIN_DIR} / "sp_trusted_client.pem";

      config::nvhttp.file_state = state_file_.string();
      config::nvhttp.sp_trusted_client_cert.clear();
      config::nvhttp.sp_disable_pin_pairing = false;

      nvhttp::test_support::reset_client_state();

      std::error_code remove_error;
      fs::remove(state_file_, remove_error);
      fs::remove(cert_file_, remove_error);
    }

    /**
     * @brief Remove test fixtures and restore the caller's configuration.
     */
    void TearDown() override {
      nvhttp::test_support::reset_client_state();

      std::error_code remove_error;
      fs::remove(state_file_, remove_error);
      fs::remove(cert_file_, remove_error);

      config::nvhttp = original_nvhttp_;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state_;
      BaseTest::TearDown();
    }

    /**
     * @brief Write a certificate fixture to the configured test path.
     *
     * @param pem PEM text to write.
     */
    void write_certificate(const std::string &pem) const {
      std::ofstream out(cert_file_, std::ios::binary);
      ASSERT_TRUE(out.is_open()) << "Failed to open " << cert_file_;
      out << pem;
      ASSERT_TRUE(out.good()) << "Failed to write " << cert_file_;
    }

  protected:
    fs::path state_file_;  ///< Task-specific persisted state fixture.
    fs::path cert_file_;  ///< Task-specific trusted client certificate fixture.

  private:
    config::nvhttp_t original_nvhttp_ {config::nvhttp};  ///< HTTP configuration restored after each test.
    bool original_fresh_state_ {config::sunshine.flags[config::flag::FRESH_STATE]};  ///< Fresh-state flag restored after each test.
  };
}  // namespace

TEST_F(SpPairingConfigTest, DefaultsAreDisabled) {
  const config::nvhttp_t defaults {};

  EXPECT_TRUE(defaults.sp_trusted_client_cert.empty());
  EXPECT_FALSE(defaults.sp_disable_pin_pairing);

  EXPECT_TRUE(config::nvhttp.sp_trusted_client_cert.empty());
  EXPECT_FALSE(config::nvhttp.sp_disable_pin_pairing);
}

TEST_F(SpPairingConfigTest, ParsesTrustedCertificateAndDisableFlag) {
  const auto cert_path = fs::path {SUNSHINE_TEST_BIN_DIR} / "parsed_trusted_client.pem";

  config::apply_config_for_test(
    "sp_trusted_client_cert = " + cert_path.string() + "\nsp_disable_pin_pairing = true\n"
  );

  EXPECT_EQ(config::nvhttp.sp_trusted_client_cert, cert_path.string());
  EXPECT_TRUE(config::nvhttp.sp_disable_pin_pairing);

  config::apply_config_for_test("sp_disable_pin_pairing = false\n");
  EXPECT_FALSE(config::nvhttp.sp_disable_pin_pairing);
}

TEST_F(SpTrustedClientTest, AuthorizesConfiguredCertificateAndRejectsUnknown) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);

  write_certificate(credentials.x509);
  config::nvhttp.sp_trusted_client_cert = cert_file_.string();

  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
}

TEST_F(SpTrustedClientTest, AuthorizesConfiguredCertificateWithFreshState) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();

  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  write_certificate(credentials.x509);
  config::nvhttp.sp_trusted_client_cert = cert_file_.string();

  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(SpTrustedClientTest, ApplyingSameCertificateTwiceDoesNotDuplicate) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();

  write_certificate(credentials.x509);
  config::nvhttp.sp_trusted_client_cert = cert_file_.string();

  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 1);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(SpTrustedClientTest, MissingOrInvalidCertificateFailsClosed) {
  config::nvhttp.sp_trusted_client_cert = (fs::path {SUNSHINE_TEST_BIN_DIR} / "does-not-exist.pem").string();
  EXPECT_FALSE(nvhttp::apply_trusted_client_cert());
  EXPECT_TRUE(nvhttp::get_all_clients().empty());

  write_certificate("not a certificate");
  config::nvhttp.sp_trusted_client_cert = cert_file_.string();
  EXPECT_FALSE(nvhttp::apply_trusted_client_cert());
  EXPECT_TRUE(nvhttp::get_all_clients().empty());
}

TEST_F(SpTrustedClientTest, EraseAllClientsReappliesTrustedCertificate) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();

  write_certificate(credentials.x509);
  config::nvhttp.sp_trusted_client_cert = cert_file_.string();

  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  ASSERT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));

  nvhttp::erase_all_clients();

  EXPECT_EQ(nvhttp::get_all_clients().size(), 1);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(SpTrustedClientTest, TrustedCertificateIsNotPersisted) {
  const auto bridge_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Gaspar Bridge");
  const auto persisted_credentials = crypto::gen_creds("Sunshine Persisted Client", 2048);

  write_certificate(bridge_credentials.x509);
  config::nvhttp.sp_trusted_client_cert = cert_file_.string();

  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  ASSERT_FALSE(nvhttp::test_support::add_client("persisted", persisted_credentials.x509, true).empty());

  EXPECT_EQ(
    config::nvhttp.sp_trusted_client_cert,
    cert_file_.string()
  );
  EXPECT_EQ(
    std::string::npos,
    file_handler::read_file(state_file_.string().c_str()).find("sp-bridge")
  );

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(bridge_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(persisted_credentials.x509));

  ASSERT_TRUE(nvhttp::apply_trusted_client_cert());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(bridge_credentials.x509));
}
