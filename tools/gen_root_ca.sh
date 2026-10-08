#!/usr/bin/env bash
# Regenerate include/ota_root_ca.h from this machine's trust store.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOTS="ISRG_Root_X1 ISRG_Root_X2 USERTrust_RSA_Certification_Authority USERTrust_ECC_Certification_Authority DigiCert_Global_Root_G2"
{
  echo '// Root CAs trusted for the OTA host (raw.githubusercontent.com).'
  echo '// Generated from the system trust store by tools/gen_root_ca.sh - do not edit by hand.'
  echo "// Several roots are pinned so a CA change on GitHub's side does not brick updates."
  echo '#pragma once'
  echo
  echo 'static const char OTA_ROOT_CA[] PROGMEM = R"CERT('
  for c in $ROOTS; do openssl x509 -in "/etc/ssl/certs/$c.pem"; done
  echo ')CERT";'
} > include/ota_root_ca.h
