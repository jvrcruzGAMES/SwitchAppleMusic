Mozilla CA trust store converted to PEM by curl's mk-ca-bundle.

Source: https://curl.se/ca/cacert.pem
Upstream documentation: https://curl.se/docs/caextract.html
Mozilla source: https://hg.mozilla.org/mozilla-central/raw-file/tip/security/nss/lib/ckfw/builtins/certdata.txt
Snapshot: 2026-11-05 (see cacert.pem header).
License: MPL-2.0; included in LICENSE.txt. The PEM file is unmodified.

To update, download cacert.pem over verified HTTPS from the same source,
check the upstream snapshot/header, and rebuild both the NRO and sysmodule
RomFS. Builds use this checked-in snapshot and do not download certificates.
Packaging verifies that both images contain the exact source bundle/license.
