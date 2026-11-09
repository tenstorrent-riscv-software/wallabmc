# HTTPS certificates

WallaBMC serves the web UI, Redfish and the websocket consoles over HTTPS.
This describes the BMC's HTTPS key and certificate, how to check and trust
them, and how to replace the certificate with one signed by your own CA.

## Security notes

- **WallaBMC isn't currently designed to keep the HTTPS private key
  private.** Anyone with the admin password, or with physical access to
  the board, can read it:
  - over the network, with the BMC shell's `devmem` command, which reads
    any memory address (see [Where the shell is
    available](#where-the-shell-is-available));
  - from the flash, which isn't encrypted or read-protected, over the
    same USB connection as the serial console (see [Key storage and local
    access](#key-storage-and-local-access)).

  With the key they can impersonate the BMC to every client that trusts
  its certificate or pins its key, and read or change that traffic. So
  HTTPS protects connections to the BMC from other people on the network,
  not from anyone with the admin password or physical access. Changing the
  admin password doesn't undo a copied key, only generating a new one does
  (see [Recovery](#recovery)).
- Anyone with the admin password can install a different certificate.
- By default the BMC shell, including the `bmc cert` commands, can be used
  over the network as well as on the serial console: the web UI's BMC Shell
  page runs it over HTTPS, after logging in with the admin password. Only
  the serial console is independent of the network, so only use it to check
  the certificate. See [Where the shell is available](#where-the-shell-is-available).
- The BMC doesn't check certificate dates, of its own certificate or
  otherwise.
- Mutual TLS (client certificate authentication) isn't supported, for
  Redfish, the web UI or the websocket consoles. The BMC doesn't ask
  clients for a certificate. Clients log in with the admin password, which
  Redfish and the web UI send as HTTP Basic auth inside the TLS connection.
  Redfish defines client certificate authentication
  (`AccountService.MultiFactorAuth.ClientCertificate`), but WallaBMC
  doesn't implement it.

## Limits

- The BMC's own key is ECDSA P-256 (see [Why the key is
  P-256](#why-the-key-is-p-256)), so its certificate has a P-256 key.
- The CA certificates in an installed chain can have P-256 or P-384 keys.
  Every certificate must be signed with ECDSA, using SHA-256 or SHA-384.
  RSA, P-521 and other curves aren't supported.
- At most 4 certificates, the BMC's certificate included, and the BMC's
  certificate itself must be at most 2 KiB (DER). A CA-issued ECDSA
  certificate is usually under 1.5 KiB.
- The BMC sends the whole chain in one TLS record, so it must fit in a sent
  record, 4 KiB: about 4 KiB of certificates (DER), less 5 bytes for each
  certificate. A chain that's too large is rejected with an HTTP error.
- The ReplaceCertificate request must also fit in a received record, 4 KiB,
  because clients send the request in one record. The chain is about 1.4
  times larger as PEM in JSON, so with these sizes the request is the
  limit, about 2.6 KiB of certificates (DER). An ECDSA certificate and one or
  two ECDSA intermediates fit. A request that's too large fails with a
  connection error rather than an HTTP error.

  See [TLS record size](#tls-record-size) for why the records are 4 KiB.

## Why the key is P-256

The BMC's key is ECDSA P-256 because it's faster than P-384. It isn't a
hardware limit: none of the supported MCUs has ECC hardware, so all ECC is
done in software by mbedTLS, which has P-384 built in to check installed
certificate chains.

P-256 is enough for this key:

- Every public CA accepts it. The CA/Browser Forum Baseline Requirements
  allow P-256, P-384 and P-521 keys.
- It gives about 128-bit security, the same as the AES-128 the BMC uses
  for TLS 1.2.
- A stronger key doesn't help while the key itself isn't protected from
  local access (see [Key storage and local
  access](#key-storage-and-local-access)).

A P-384 key costs about 100 ms more for every new HTTPS connection.
Measured on nucleo_f767zi (STM32F767 at 216 MHz), with the same firmware
apart from the key's curve:

| | P-256 key | P-384 key | Change |
|---|---|---|---|
| Generate the key (once) | 193 ms | 338 ms | +75% |
| Issue the self-signed certificate (one signature) | 120 ms | 215 ms | +79% |
| TLS 1.3 handshake | 506 ms | 604 ms | +98 ms (+19%) |
| TLS 1.2 handshake | 541 ms | 639 ms | +98 ms (+18%) |
| 5 new connections at once | 2.59 s | 3.08 s | +0.49 s (+19%) |

Notes on the measurements:

- Handshake times are curl's `time_appconnect - time_connect`, averaged
  over 30 connections from a client on the same network. Key exchange was
  ECDHE with P-256 throughout.
- A P-384 signature takes about 1.8 times as long as a P-256 one. Most of a
  handshake is the P-256 key exchange, which is the same in both cases, so
  a handshake is about a fifth slower.
- The BMC does one handshake at a time, so a browser that opens several
  connections at once pays the difference for each.
- Resumed TLS 1.3 sessions don't sign, so don't pay the difference.
- The extra mbedTLS heap needed for a P-384 key is negligible (228 bytes
  at the peak, with 5 connections).

A P-384 key would also stop TLS 1.2 clients that only offer P-256 from
connecting.

Some organisations require P-384 keys, eg. those following NSA's CNSA
policy. WallaBMC can't meet those policies: its key is P-256, and TLS 1.2
uses AES-128.

## How it works

On first boot each BMC generates its own ECDSA P-256 private key, using the
hardware random number generator, and saves it in its configuration flash.
The firmware never sends the key anywhere, and no two BMCs share a key. The
firmware image contains no key or certificate. The key can still be read
with local access (see [Key storage and local
access](#key-storage-and-local-access)).

The BMC then issues itself a self-signed certificate for:

- its hostname (`config bmc hostname`, Redfish `HostName`, or the web UI),
- its static IPv4 address, if one is set,
- any extra names set with `bmc cert names` (see [Names](#names)).

Its contents are listed in [Self-signed certificate
format](#self-signed-certificate-format).

The certificate has fixed dates, valid from 2000-01-01 until 9999-12-31,
so in practice it doesn't expire (see [Certificate dates and the BMC's
clock](#certificate-dates-and-the-bmcs-clock)). Its fingerprint is printed
on the serial console at every boot:

```
<inf> wallabmc_https_cert: HTTPS certificate (self-signed): CN=wallabmc, O=WallaBMC, serialNumber=6f1c0b2e-8d3a-5f47-9c21-4e7b3a90d5c8
<inf> wallabmc_https_cert:   Names: DNS:wallabmc
<inf> wallabmc_https_cert:   SHA-256 fingerprint: 87:9B:CD:93:...
<inf> wallabmc_https_cert:   Public key pin: sha256//mJxa85wVksOMNOfwV7w5unvpHEX1/9znMgV2IhIEpiw=
```

## Self-signed certificate format

The self-signed certificate always has this format:

| Field | Value |
|---|---|
| Version | X.509 v3 |
| Serial number | 16 random bytes with the top bit cleared (127 bits), new each time the certificate is reissued |
| Signature algorithm | `ecdsa-with-SHA256` |
| Issuer | Same as the subject |
| Subject | `CN=<hostname>, O=WallaBMC, serialNumber=<BMC UUID>` |
| Not before | 2000-01-01 00:00:00 UTC |
| Not after | 9999-12-31 23:59:59 UTC (`99991231235959Z`, no expiry) |
| Public key | ECDSA P-256, the BMC's own key |
| Basic constraints | Critical, `CA:FALSE` |
| Key usage | Critical, `digitalSignature` |
| Extended key usage | Critical, `serverAuth` |
| Subject key identifier | SHA-1 hash of the public key ([RFC 5280 4.2.1.2](https://www.rfc-editor.org/rfc/rfc5280#section-4.2.1.2), method 1) |
| Authority key identifier | Same as the subject key identifier |
| Subject alternative name | The hostname, the static IPv4 address if one is set, and any extra names set with `bmc cert names` |

To see it, run:

```
openssl s_client -connect wallabmc:443 </dev/null 2>/dev/null |
    openssl x509 -noout -text
```

Notes on the fields:

- **Serial number:** it's random so a reissued certificate never has the
  same issuer and serial number as an earlier one. Firefox rejects a
  certificate with the same issuer and serial number as a different one it
  has seen before.
- **Subject:** the BMC's UUID is the one Redfish shows as the manager's
  `UUID`. It's there because every BMC has the hostname `wallabmc` until
  it's changed. Without it, every BMC's certificate would have the same
  subject and issuer, which can confuse clients that keep certificates by
  issuer.
- **Dates:** they're fixed, see [Certificate dates and the BMC's
  clock](#certificate-dates-and-the-bmcs-clock).
- **Extended key usage:** it's critical because mbedTLS always writes it
  that way. Certificates from public CAs usually have it non-critical. [RFC
  5280](https://www.rfc-editor.org/rfc/rfc5280#section-4.2.1.12) allows
  either, and it makes no difference to clients that understand the
  extension, which all TLS clients do.
- **Subject alternative name:** addresses from DHCP aren't included, see
  [Names](#names).

The certificate is reissued, with the same key, when the hostname, the
static IPv4 address or the extra names change, and with `bmc cert
self-signed` or a Redfish `DELETE` of the certificate (see
[Recovery](#recovery)). Only the serial number and the signature are
different each time; the rest only changes with the names.

## Certificate dates and the BMC's clock

The self-signed certificate is always valid from 2000-01-01 00:00:00 UTC
until 9999-12-31 23:59:59 UTC. These dates are fixed: they don't depend on
the BMC's clock, and they're the same every time the certificate is
reissued. The end date is the value [RFC
5280](https://www.rfc-editor.org/rfc/rfc5280#section-4.1.2.5) specifies for
a certificate with no well-defined expiration date.

**The BMC never has a trusted clock.**

- At the first boot, or after its clock is lost, the clock starts from a
  fixed date, 2026-01-01 on boards with an RTC.
- nucleo_f767zi's RTC isn't battery-backed, so it loses the time whenever
  the power is removed. It keeps the time across a reset.
- The time is set by NTP (SNTP), if it's enabled and a server is
  reachable, or through Redfish (`DateTime`) or the web UI. SNTP isn't
  authenticated, so anyone on the network path to the NTP server can set
  the BMC's clock.

A certificate dated from that clock could have any dates. If its start
date were in the future, clients would reject it until then.

**The certificate isn't reissued with real dates once the time is known:**

- An expiry date adds no protection to a self-signed certificate. Clients
  trust that certificate, or pin its key, and nobody can revoke it. The way
  to stop trusting a key is to generate a new one (`bmc cert regenerate`,
  see [Recovery](#recovery)), and remove it from the clients.
- A real expiry date would need the BMC to renew the certificate
  automatically. Each renewal changes the certificate, so every browser
  exception and saved certificate would break with it. Only key pins would
  survive.
- The dates would come from a clock that can't be trusted, as above.

**The two dates have to be chosen together.** Apple's platforms (iOS 13,
macOS 10.15 and later) reject a TLS server certificate that's valid for
more than 825 days. That only applies to certificates with a start date
after 2019-07-01 ([Apple's requirements for trusted
certificates](https://support.apple.com/en-us/103769)). With a start date of
2000-01-01, the certificate is outside that rule, and Safari accepts it
(see [Client compatibility](#client-compatibility)). A recent start date
with the 9999 end date would be rejected by Safari and other Apple clients.
Chrome's 398-day limit only applies to certificates from publicly trusted
CAs.

If you need certificates that expire and are renewed, use a certificate
signed by your CA (see [Using a certificate signed by your
CA](#using-a-certificate-signed-by-your-ca)). The CA sets its dates.

The BMC doesn't check any certificate dates: mbedTLS is built without
`MBEDTLS_HAVE_TIME_DATE`. It installs a certificate that has expired or
isn't valid yet, and doesn't warn when an installed certificate is about to
expire. Monitor `ValidNotAfter` in Redfish instead (see
[Renewal](#renewal)).

## Checking the certificate

A self-signed certificate can't prove who it belongs to, so before trusting
it, check it through a channel an attacker can't intercept: the BMC's serial
console, the board's UART or USB serial connection. Run `bmc cert`:

```
uart:~$ bmc cert
Source:              self-signed
Subject:             CN=wallabmc, O=WallaBMC, serialNumber=6f1c0b2e-8d3a-5f47-9c21-4e7b3a90d5c8
Names:               DNS:wallabmc
SHA-256 fingerprint: 87:9B:CD:93:AE:27:12:A4:F6:B2:14:E5:23:97:D3:5D:FF:56:3F:E9:CE:89:11:6C:B0:80:03:3B:0D:21:34:8B
Public key pin:      sha256//mJxa85wVksOMNOfwV7w5unvpHEX1/9znMgV2IhIEpiw=
Extra names:         (none)
```

and compare the fingerprint with the one your browser or client shows, or
with:

```
openssl s_client -connect wallabmc:443 </dev/null 2>/dev/null |
    openssl x509 -noout -fingerprint -sha256
```

Don't check it with anything that comes over the network. That includes
the fingerprint under BMC Settings in the web UI, and `bmc cert` run in the
web UI's BMC Shell page. Both come over the HTTPS connection you're trying
to verify, so an attacker who has intercepted it can show you the
fingerprint of their own certificate.

## Trusting the certificate

Save the certificate once you've checked it:

```
openssl s_client -connect wallabmc:443 </dev/null 2>/dev/null |
    openssl x509 > wallabmc.pem
```

- **Browsers:** accept the warning and add an exception, or import
  `wallabmc.pem` into the browser or operating system trust store. An
  exception is tied to the certificate, so it has to be accepted again when
  the certificate is reissued (see [Names](#names)). An imported
  certificate has to be imported again too.
- **Chrome:** import `wallabmc.pem` at `chrome://certificate-manager`.
  On Linux, Chrome doesn't use the system's `ca-certificates`, so adding
  it there isn't enough. If you clicked through the certificate warning
  before importing it, Chrome still shows "Not secure" after the import.
  DevTools says "you have recently allowed content loaded with certificate
  errors to run on this site". To clear that, delete the site's data
  (Site settings → Delete data) and restart Chrome.
- **Firefox:** add a security exception. Either click **Accept the Risk
  and Continue** on the warning page, or use Settings → Privacy & Security
  → Certificates → View Certificates → Servers → **Add Exception…**, which
  shows the certificate, so you can check its fingerprint before trusting
  it. An exception is for one name or address, so add one for each you
  use. Firefox never shows a site it trusts through an exception as fully
  secure in the URL bar, but the connection is verified against the
  certificate you accepted. Firefox doesn't use the system's
  `ca-certificates` either, and won't import the certificate as an
  authority, because it isn't a CA certificate.
- **Safari (macOS):** Safari uses the macOS keychain, so add
  `wallabmc.pem` to it and trust it for TLS:

  ```
  security add-trusted-cert -r trustRoot -p ssl \
      -k ~/Library/Keychains/login.keychain-db wallabmc.pem
  ```

  That trusts it for your user. Or in Keychain Access, drag `wallabmc.pem`
  into the login keychain, open it, and under **Trust** set **Secure
  Sockets Layer (SSL)** to **Always Trust**. Then reload the page; quit
  and reopen Safari if it had already shown the warning. **Show Details →
  visit this website** on Safari's warning page also adds the certificate
  to the keychain, but without showing you its fingerprint first. To
  remove it, for example after the certificate is reissued, delete it in
  Keychain Access.
- **Safari (iOS, iPadOS):** send `wallabmc.pem` to the device, eg. with
  AirDrop, install it as a profile in Settings, then turn it on under
  Settings → General → About → **Certificate Trust Settings**.
- **curl:** `curl --cacert wallabmc.pem https://wallabmc/redfish/v1`, or pin
  the key with `curl -k --pinnedpubkey 'sha256//<pin>' ...` using the public
  key pin from `bmc cert`. `-k` is needed because the pin is checked as well
  as the certificate, not instead of it. Despite the `-k`, this is as safe as
  `--cacert`: curl still checks the pin, and drops the connection before
  sending anything if the server's key doesn't match, so an attacker would
  need the BMC's private key to impersonate it. The pin stays the same when
  the certificate is reissued.
- **Python requests:** `requests.get(url, verify="wallabmc.pem")`.
- **redfishtool** (1.1.8) doesn't verify the BMC's certificate at all: it
  always calls requests with `verify=False`. It protects the connection
  from eavesdropping, but not from an attacker who intercepts it, who
  could also capture the password.

Verification checks the name you connect with against the certificate's
names, so connect with the hostname, or a name or address in the
certificate.

## Client compatibility

Clients tested with the self-signed certificate. Each client was given
`wallabmc.pem` as its only trusted certificate, and connected by the
hostname (`https://wallabmc/`) and by IP address. Each was also checked to
reject the certificate when it isn't trusted, to show it really verifies
it.

Tested on 2026-11-17, and Safari on 2026-11-21, against nucleo_f767zi. The
command-line clients ran on Ubuntu 26.04, and the browsers on a desktop:

| Client | Version, TLS library | By name | By IP | Notes |
|---|---|---|---|---|
| curl | 8.18.0, OpenSSL 3.5.5 | ✅ | ✅ | `--cacert`. `-k --pinnedpubkey` also works, and rejects a wrong pin |
| wget | 1.25.0, GnuTLS | ✅ | ✅ | `--ca-certificate` |
| hurl | 7.1.0, libcurl 8.18.0 | ✅ | ✅ | `--cacert` |
| Python requests | 2.32.5, OpenSSL 3.5.5 | ✅ | ✅ | `verify=` |
| openssl s_client | 3.5.5 | ✅ | ✅ | `-CAfile`, `-verify_hostname` / `-verify_ip` |
| gnutls-cli | 3.8.12 | ✅ | ✅ | `--x509cafile`. Warns "There was a non-CA certificate in the trusted list", but verifies |
| Go net/http | 1.26.8, crypto/tls | ✅ | ✅ | `tls.Config.RootCAs` |
| Node.js https | 22.22.1, OpenSSL 3.5.5 | ✅ | ✅ | `ca:` |
| redfishtool | 1.1.8 | n/a | n/a | Doesn't verify certificates, see [Trusting the certificate](#trusting-the-certificate) |
| Chrome | 157.0.8103.38, BoringSSL | ✅ | ✅ | Imported at `chrome://certificate-manager`, see [Trusting the certificate](#trusting-the-certificate) |
| Firefox | 159.0.1, NSS | ✅ | ✅ | Security exception, see [Trusting the certificate](#trusting-the-certificate). The URL bar never shows the site as fully secure |
| Safari, macOS | 27.2, macOS 26.7.3 | ✅ | ✅ | Trusted with `security add-trusted-cert`, see [Trusting the certificate](#trusting-the-certificate). No warning about the validity period, the 825-day rule doesn't apply |
| Edge | | | | Not tested yet |

For browsers, check:

- the certificate can be trusted, with an exception or by importing it,
  and the page then loads with no warning, by name and by IP address;
- there's no error about the certificate's validity period, eg. Chrome's
  `NET::ERR_CERT_VALIDITY_TOO_LONG`, or Safari saying the certificate isn't
  standards compliant.

Installing CA-signed chains, and verifying them with curl and openssl
s_client, is tested by `scripts/test-https-cert.sh`.

## Names

The self-signed certificate is reissued automatically, with the same key,
when the hostname or static IPv4 address changes. New connections get the new
certificate straight away. A key pin stays valid; browser exceptions and
saved certificates have to be updated.

Addresses given to the BMC by DHCP aren't put in the certificate, otherwise
it would change with every new lease, and every browser exception and saved
certificate would break with it. The BMC also doesn't request its DNS domain
from DHCP, and couldn't be sure a name in it points at the BMC. So add any
other names clients use with `bmc cert names`. DNS names and IPv4 addresses
can be given, up to 8:

```
uart:~$ bmc cert names bmc1.lab.example.com 192.168.1.110
uart:~$ bmc cert names           # show them
uart:~$ bmc cert names clear     # remove them
```

If you need to connect by address, use a DHCP reservation and add the
address as an extra name.

Wildcards, like `*.lab.example.com`, aren't allowed, in the extra names or
in a CSR. The key isn't protected from local access (see [Key storage and
local access](#key-storage-and-local-access)). With a wildcard
certificate, anyone who reads the key from one BMC could impersonate every
host under that domain to clients that trust the certificate, not just
the BMC. Give each name the BMC is reached by instead.

With a certificate signed by your CA installed, `bmc cert names` still saves
the names, but doesn't change the installed certificate. It prints the names
the certificate isn't valid for:

```
uart:~$ bmc cert names bmc1.lab.example.com 10.0.0.5
Extra names: bmc1.lab.example.com,10.0.0.5
Certificate is not valid for: 10.0.0.5
```

The names are used:

- by GenerateCSR, if the request has no `AlternativeNames`, so the next CSR
  includes them;
- in the self-signed certificate, if you go back to one (see
  [Recovery](#recovery)).

A hostname is a single DNS label of up to 63 letters, digits and `-`.

## Using a certificate signed by your CA

The BMC can create a certificate signing request (CSR) for its key, which
your CA signs. The key doesn't change, so the BMC stays reachable with the
old certificate until the new one is installed. This is the standard Redfish
CertificateService process, as used by other BMCs. The web UI has the same
steps under BMC Settings, HTTPS Certificate.

⚠️ **Your CA must sign with ECDSA, using SHA-256 or SHA-384, and every
certificate in the chain must have a P-256 or P-384 key.** The BMC can't
parse RSA certificates or other curves, and rejects them. See
[Limits](#limits) for the chain size.

### 1. Generate a CSR

```
curl --cacert wallabmc.pem -u admin:<password> \
    -H 'Content-Type: application/json' \
    https://wallabmc/redfish/v1/CertificateService/Actions/CertificateService.GenerateCSR \
    -d '{
          "CertificateCollection": {
            "@odata.id": "/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates"
          },
          "CommonName": "bmc1.lab.example.com",
          "Organization": "Example Inc",
          "Country": "US",
          "AlternativeNames": ["bmc1.lab.example.com", "wallabmc"]
        }' | jq -r .CSRString > bmc1.csr
```

`CommonName` is required. If `AlternativeNames` is left out, the CSR has the
BMC's current names. The common name is always added to the alternative
names. Neither can be a wildcard (see [Names](#names)). `KeyPairAlgorithm`,
`KeyCurveId` and `KeyBitLength` can only ask for the existing key:
`TPM_ALG_ECDSA`, `TPM_ECC_NIST_P256` and 256.

### 2. Sign it

With an openssl CA, include the names from the CSR:

```
cat > bmc1.ext <<EOF
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:bmc1.lab.example.com, DNS:wallabmc
EOF
openssl x509 -req -in bmc1.csr -CA ca.pem -CAkey ca.key -days 365 \
    -extfile bmc1.ext -out bmc1.pem
```

With [step-ca](https://smallstep.com/docs/step-ca/), whose keys are P-256 by
default:

```
step ca sign bmc1.csr bmc1.pem
```

With a public CA, if the name is in public DNS, eg. with certbot and a DNS
challenge:

```
certbot certonly --csr bmc1.csr --manual --preferred-challenges dns
```

Check every certificate you install is ECDSA. `openssl x509 -noout -text`
should show `ecdsa-with-SHA256` or `ecdsa-with-SHA384`, and `prime256v1` or
`secp384r1`, for each certificate. Some CAs add a cross-signed certificate,
signed with RSA, to the chain; leave that out.

### 3. Install it

Install the certificate followed by any intermediate certificates, but not
the root:

```
cat bmc1.pem intermediate.pem > bmc1-chain.pem
jq -n --rawfile c bmc1-chain.pem '{
    CertificateUri: {
      "@odata.id": "/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates/1"
    },
    CertificateType: "PEM",
    CertificateString: $c
  }' |
curl --cacert wallabmc.pem -u admin:<password> \
    -H 'Content-Type: application/json' --data-binary @- \
    https://wallabmc/redfish/v1/CertificateService/Actions/CertificateService.ReplaceCertificate
```

The BMC checks the certificate is for its key, that each certificate in the
chain is the issuer of the one before and signed it, and that the chain fits
in a TLS record (see [Limits](#limits)). Expiry dates aren't checked. If a
check fails, it returns an error saying why and keeps the current
certificate. Otherwise new connections use the new certificate immediately,
and it's saved for later boots.

Clients can then verify the BMC with your CA's root certificate:

```
curl --cacert ca.pem https://bmc1.lab.example.com/redfish/v1
```

An installed certificate is never replaced automatically. If the hostname,
static address or extra names are changed to a name it doesn't cover, the
BMC logs a warning, and `bmc cert` and `bmc cert names` show it:

```
Certificate is not valid for: bmc2
```

A fully qualified name covers the hostname it starts with, eg. a certificate
for `bmc1.lab.example.com` covers the hostname `bmc1`.

### Renewal

The BMC doesn't check certificate expiry. Repeat the steps above before the
certificate expires: generate a CSR, sign it and install it. Public CA
certificates are limited to 398 days, and the limit is getting shorter, so
automate renewal off the BMC if you use one.

The certificate is shown in Redfish at
`/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates/1`, including
`ValidNotAfter`, so monitoring can check when it expires.

## Firmware updates

The key and certificate are kept in the configuration flash, not in the
firmware image. So they normally stay the same across firmware updates,
and clients that trust the certificate or pin the key keep working. That
holds as long as all of these are true:

- **The update only writes the firmware image.** `west flash`, and
  `esptool write-flash` of the image, only write the image. A full chip
  erase also erases the configuration, eg. `esptool erase-flash`, a mass
  erase from STM32CubeProgrammer or OpenOCD, or `west flash --erase`. A new
  key is then generated at the next boot.
- **The flash layout doesn't change.** The configuration is in the
  `storage` partition (on nucleo_f767zi at 0x8000, on xiao_esp32c6 at
  0x3b0000). Firmware with a different partition layout won't find it.
- **The configuration version doesn't change.** If the firmware's
  configuration version (`CFG_CURRENT_VERSION` in `src/config.c`) differs
  from the stored one, the firmware clears the whole configuration at boot,
  including the key. That applies to downgrades as well as upgrades.
- **The item IDs aren't reused.** The key and certificate are stored under
  fixed IDs, and `src/config.c` requires that IDs are never reused or
  changed in meaning.

There's no automated test for this. To check an update kept the key,
compare the public key pin that `bmc cert` shows, or that's logged at boot,
before and after.

### What's checked at boot

At every boot, before HTTPS starts, the BMC:

1. Parses the stored key, which must be an ECDSA signing key.
2. Derives the public key from the private key, and checks it matches the
   public key stored with it (`mbedtls_pk_check_pair()`). This takes about
   90 ms on nucleo_f767zi.
3. Parses the stored certificate, and checks its public key is the key's.
   This applies to an installed certificate as well as a self-signed one.
4. Checks a self-signed certificate is for the current names, and
   reissues it if not (see [Names](#names)).

If the key fails step 1 or 2, it's corrupt. The BMC logs an error and
generates a new key, so clients that trusted or pinned the old one must be
updated. If the certificate fails step 3, the BMC logs a warning and issues
a self-signed certificate for the key. That replaces an installed
certificate, which has to be installed again.

The BMC doesn't check the installed chain's signatures at boot; that's only
checked when it's installed. It never checks dates.

## Key storage and local access

The key is stored as plain SEC1 DER in the configuration flash (NVS), with
the rest of the configuration. While the BMC runs, it's also in RAM, and
every HTTPS connection has a parsed copy. It isn't encrypted, and isn't
held in any hardware key store.

HTTPS doesn't protect the key from anyone with the admin password, who can
read it with the shell's `devmem` command (see [Where the shell is
available](#where-the-shell-is-available)), or from someone with local
access. Anyone who can reach the board's debug or flashing interface can
read the key, and then impersonate the BMC to every client that trusts its
certificate or pins its key. In practice that's anyone who can plug in the
USB cable used for the serial console.

| Board | HTTPS key | Local access to the key |
|---|---|---|
| nucleo_f767zi | Internal flash | The on-board ST-Link provides the serial console and SWD over the same USB port. SWD can read the flash and RAM. |
| xiao_esp32c6 | Flash | The USB-C port provides the console, JTAG and the ROM download mode. `esptool` or JTAG can read the flash. |
| hifive_premier_p550_mcu | None, HTTPS is disabled | n/a |
| qemu_cortex_m3 | None, HTTPS is disabled | n/a |

Flash protection isn't a meaningful protection on any of these boards, and
WallaBMC doesn't enable any:

- **STM32 (nucleo_f767zi):** WallaBMC leaves read-out protection (RDP) at
  level 0, the factory default, which is no protection.
  - Level 1 stops a debugger reading the flash, but not RAM. The running
    firmware keeps the key in RAM, so it can still be read from a running
    board.
  - Level 2 disables the debug port permanently, and can't be undone. That
    also removes the way the board is flashed and debugged. Voltage
    glitching attacks that downgrade level 2 have been published for
    similar STM32 parts.
  - The STM32F767 has no hardware key storage or device-unique secret key.
    Encrypting the key would need a second key stored in the same flash,
    which protects nothing.
- **ESP32-C6 (xiao_esp32c6):** the chip supports flash encryption and secure
  boot, with the encryption key in eFuses that software can't read. WallaBMC
  doesn't configure them. Enabling them burns eFuses, which can't be undone,
  and changes how the board is flashed.

Treat the key as no better protected than the board itself.

### Retiring or transferring a BMC

Before a board leaves your control, run `config clear_and_reboot` (or
press the reset button if the board has one). That erases the whole
configuration flash, including the key, the admin password and the network
settings.

`bmc cert regenerate` isn't enough. It makes a new key, but it only marks
the old one deleted, and the old key's bytes stay in flash until the space
is reused.

If a board may have been accessed by someone else, assume its key is known:

- Run `bmc cert regenerate` on it.
- Remove the old certificate or pin from every client that trusted it.
- If the certificate was signed by your CA, revoke it.

## Recovery

- **Back to self-signed:** `bmc cert self-signed` replaces an installed
  certificate, eg. a wrong or expired one, with a self-signed one for the
  current names. The key doesn't change. A Redfish
  `DELETE` of the certificate does the same, there's always a certificate.
  The installed certificate may not verify, but the key is unchanged, so pin
  it rather than send the password with `-k` alone:

  ```
  curl -k --pinnedpubkey 'sha256//<pin>' -u admin:<password> -X DELETE \
      https://wallabmc/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates/1
  ```
- **New key:** `bmc cert regenerate` deletes the key and certificate,
  clears the key from RAM, and reboots. A new key and self-signed
  certificate are generated at boot. Clients that trusted or pinned the old
  one must be updated.

  **The old key isn't wiped from flash.** It's only marked deleted, and its
  bytes stay in the configuration flash, where anyone with local access can
  read them (see [Key storage and local
  access](#key-storage-and-local-access)). They stay there until the space
  is reused, which may be never. If the old key has to be wiped, eg. before
  the board leaves your control, use `config clear_and_reboot` instead. It
  erases the whole configuration, which also generates a new key.
- **Factory reset:** `config clear_and_reboot`, or the reset button if the
  board has one, clears all configuration, including the key, so a new key
  is generated at boot.

If the BMC can't load or generate a key at boot, it logs an error and HTTPS
isn't available. It never falls back to a built-in key.

## TLS record size

The 4 KiB record sizes are set by `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` and
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN`, for records received and sent, to
save RAM. TLS allows records of up to 16 KiB, but each HTTPS connection has
a receive and a send buffer the size of a record, allocated from the mbedTLS
heap, and browsers open several connections at once. At 4 KiB a connection's
buffers take about 9 KiB, and 5 connections peak at about 44 KiB of heap. At
16 KiB they would take about 33 KiB per connection, and about 170 KiB for 5
connections, which is more RAM than most of the supported boards have spare.
Clients mostly send small requests, so the 4 KiB limit is only noticeable
when installing a certificate.

The largest certificate chain that can be installed, and the buffer for the
ReplaceCertificate request, follow `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN`: the
chain has to fit in a sent record. The request buffer is also never bigger
than a received record. The BMC's certificate itself stays at most 2 KiB
(DER), or less if a sent record is smaller.

## Where the shell is available

The BMC shell, with all of its commands, can be reached in these ways:

| Where | Authentication | Encrypted | Enabled |
|---|---|---|---|
| Serial console (the board's UART or USB serial) | None, physical access | n/a, local | Always |
| Web UI BMC Shell page, `wss://<bmc>/console/bmc` (port 443) | Admin password | Yes, TLS | By default, `CONFIG_APP_WEB_TERMINAL_BMC_SHELL` |
| `ws://<bmc>/console/bmc` (port 80) | Admin password, sent in clear text | No | Only with `CONFIG_APP_HTTP=y`, which is off by default when HTTPS is enabled |
| Telnet (port 23) | None | No | Only with `CONFIG_SHELL_BACKEND_TELNET=y`, which only the QEMU CI build uses |

So by default the shell is available on the same network as the web UI
and Redfish, and nothing restricts the `bmc cert` commands to the serial
console.

**The network shell can be used to get around HTTPS.** It runs inside an
HTTPS connection and needs the admin password, but it includes Zephyr's
`devmem` command (`CONFIG_DEVMEM_SHELL`, on by default), which reads and
writes any memory address. Anyone with the admin password can use it to:

- read the private key, from RAM or from the configuration flash;
- change the running firmware's code and data.

With the key they can impersonate the BMC. A copied key keeps working
after the admin password is changed, until a new key is generated with
`bmc cert regenerate` and every client is updated. This follows from
WallaBMC not currently being designed to keep the key private (see
[Security notes](#security-notes)), rather than being a separate bug.

Some things can only be done from the shell. With the default build that
includes the network shell, so anyone with the admin password can do them
remotely:

- generate a new key (`bmc cert regenerate`);
- set the extra names (`bmc cert names`);
- clear all configuration, including the key (`config clear_and_reboot`).

The network shell can't be used to check the certificate. Its output comes
over the connection being checked (see [Checking the
certificate](#checking-the-certificate)).

To keep the shell off the network:

- Build with `CONFIG_APP_WEB_TERMINAL_BMC_SHELL=n`, and `CONFIG_SHELL=y`
  to keep the shell on the serial console.
- Leave `CONFIG_APP_HTTP` and `CONFIG_SHELL_BACKEND_TELNET` disabled.

The serial console is then the only way to reach the shell. That removes
one way of reading the key, but doesn't make the key private: it can still
be read with physical access. Don't enable
telnet on a BMC on an untrusted network: it gives anyone who can connect a
shell with no password, in clear text.

## Shell commands

These work in the shell wherever it's used (see [Where the shell is
available](#where-the-shell-is-available)), but only check the certificate
with `bmc cert` on the serial console.

| Command | |
|---|---|
| `bmc cert` | Show the certificate, its fingerprint and public key pin |
| `bmc cert names [<name>... \| clear]` | Show or set extra names |
| `bmc cert self-signed` | Replace an installed certificate with a self-signed one |
| `bmc cert regenerate` | Delete the key and certificate and reboot to generate new ones. The old key stays in flash; use `config clear_and_reboot` to wipe it |
