# HTTPS certificates

WallaBMC serves the web UI, Redfish and the websocket consoles over HTTPS.
This describes the BMC's HTTPS key and certificate, how to check and trust
them, and how to replace the certificate with one signed by your own CA.

## Security notes

- The private key is stored unencrypted in the MCU's internal flash, with the
  rest of the configuration. Anyone with physical or debugger (JTAG/SWD)
  access can read it. On STM32, enable flash read-out protection (RDP) to
  prevent that.
- Anyone with the admin password can install a different certificate.
- The BMC doesn't check certificate dates, of its own certificate or
  otherwise.

## Limits

- Only ECDSA P-256 keys and ECDSA signatures are supported, for the
  certificate and the whole chain.
- At most 4 certificates, the BMC's certificate included, and the BMC's
  certificate itself must be at most 2 KiB (DER). A CA-issued ECDSA
  certificate is usually under 1.5 KiB.
- The whole chain must fit in one TLS record, 4 KiB. In practice the
  ReplaceCertificate request is the limit: clients send the request in one
  TLS record, so it must be under 4 KiB, which allows about 2.6 KiB of
  certificates (DER). An ECDSA certificate and one or two ECDSA intermediates
  fit. A request that's too large fails with a connection error rather than
  an HTTP error. See [TLS record size](#tls-record-size) for why.

## How it works

On first boot each BMC generates its own ECDSA P-256 private key, using the
hardware random number generator, and saves it in its configuration flash.
The key never leaves the BMC, and no two BMCs share a key. The firmware image
contains no key or certificate.

The BMC then issues itself a self-signed certificate for:

- its hostname (`config bmc hostname`, Redfish `HostName`, or the web UI),
- its static IPv4 address, if one is set,
- any extra names set with `bmc cert names` (see [Names](#names)).

The certificate has no expiry date, because the BMC has no trusted clock
when it first boots. It's valid until 9999-12-31, which is the value
[RFC 5280](https://www.rfc-editor.org/rfc/rfc5280#section-4.1.2.5) specifies
for a certificate with no well-defined expiration date. Its fingerprint is
printed on the serial console at every boot:

```
<inf> wallabmc_https_cert: HTTPS certificate (self-signed): CN=wallabmc, O=WallaBMC
<inf> wallabmc_https_cert:   Names: DNS:wallabmc
<inf> wallabmc_https_cert:   SHA-256 fingerprint: 87:9B:CD:93:...
<inf> wallabmc_https_cert:   Public key pin: sha256//mJxa85wVksOMNOfwV7w5unvpHEX1/9znMgV2IhIEpiw=
```

## Checking the certificate

A self-signed certificate can't prove who it belongs to, so before trusting
it, check it through a channel an attacker can't intercept: the BMC's serial
console. Run `bmc cert`:

```
uart:~$ bmc cert
Source:              self-signed
Subject:             CN=wallabmc, O=WallaBMC
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

The web UI also shows the fingerprint, under BMC Settings, but that's
convenience only: it comes over the connection you're trying to verify.

## Trusting the certificate

Save the certificate once you've checked it:

```
openssl s_client -connect wallabmc:443 </dev/null 2>/dev/null |
    openssl x509 > wallabmc.pem
```

- **Browsers:** accept the warning and add an exception, or import
  `wallabmc.pem` into the browser or operating system trust store. An
  exception is tied to the certificate, so it has to be accepted again when
  the certificate is reissued (see [Names](#names)).
- **curl:** `curl --cacert wallabmc.pem https://wallabmc/redfish/v1`, or pin
  the key with `curl -k --pinnedpubkey 'sha256//<pin>' ...` using the public
  key pin from `bmc cert`. `-k` is needed because the pin is checked as well
  as the certificate, not instead of it. Despite the `-k`, this is as safe as
  `--cacert`: curl still checks the pin, and drops the connection before
  sending anything if the server's key doesn't match, so an attacker would
  need the BMC's private key to impersonate it. The pin stays the same when
  the certificate is reissued.
- **Python requests:** `requests.get(url, verify="wallabmc.pem")`.

Verification checks the name you connect with against the certificate's
names, so connect with the hostname, or a name or address in the
certificate.

## Names

The self-signed certificate is reissued automatically, with the same key,
when the hostname or static IPv4 address changes. New connections get the new
certificate straight away. A key pin stays valid; browser exceptions and
saved certificates have to be updated.

Addresses given to the BMC by DHCP aren't put in the certificate, otherwise
it would change with every new lease, and every browser exception and saved
certificate would break with it. The BMC also doesn't request its DNS domain
from DHCP, and couldn't be sure a name in it points at the BMC. So add any
other names clients use with `bmc cert names`. DNS names (including
wildcards like `*.lab.example.com`) and IPv4 addresses can be given, up to 8:

```
uart:~$ bmc cert names bmc1.lab.example.com 192.168.1.110
uart:~$ bmc cert names           # show them
uart:~$ bmc cert names clear     # remove them
```

If you need to connect by address, use a DHCP reservation and add the
address as an extra name.

A hostname is a single DNS label of up to 63 letters, digits and `-`.

## Using a certificate signed by your CA

The BMC can create a certificate signing request (CSR) for its key, which
your CA signs. The key doesn't change, so the BMC stays reachable with the
old certificate until the new one is installed. This is the standard Redfish
CertificateService process, as used by other BMCs. The web UI has the same
steps under BMC Settings, HTTPS Certificate.

⚠️ **Your CA must sign with ECDSA, and every certificate in the chain must have
a P-256 key.** The BMC can't parse RSA certificates or other curves, and
rejects them. See [Limits](#limits) for the chain size.

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
names. `KeyPairAlgorithm`, `KeyCurveId` and `KeyBitLength` can only ask for
the existing key: `TPM_ALG_ECDSA`, `TPM_ECC_NIST_P256` and 256.

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

Check the CA's chain is ECDSA (`openssl x509 -noout -text` shows
`ecdsa-with-SHA256` and `prime256v1` for each certificate).

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

An installed certificate is never replaced automatically. If the hostname or
static address is changed to a name it doesn't cover, the BMC logs a warning
and `bmc cert` shows it:

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

## Recovery

- **Back to self-signed:** `bmc cert self-signed` on the serial console
  replaces an installed certificate, eg. a wrong or expired one, with a
  self-signed one for the current names. The key doesn't change. A Redfish
  `DELETE` of the certificate does the same, there's always a certificate.
  The installed certificate may not verify, but the key is unchanged, so pin
  it rather than send the password with `-k` alone:

  ```
  curl -k --pinnedpubkey 'sha256//<pin>' -u admin:<password> -X DELETE \
      https://wallabmc/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates/1
  ```
- **New key:** `bmc cert regenerate` deletes the key and certificate and
  reboots. A new key and self-signed certificate are generated at boot.
  Clients that trusted or pinned the old one must be updated.
- **Factory reset:** `config clear_and_reboot`, or the reset button if the
  board has one, clears all configuration, including the key, so a new key
  is generated at boot.

If the BMC can't load or generate a key at boot, it logs an error and HTTPS
isn't available. It never falls back to a built-in key.

## TLS record size

The 4 KiB record size is set by `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN`, to
save RAM. TLS allows records of up to 16 KiB, but each HTTPS connection has
a receive and a send buffer the size of a record, allocated from the mbedTLS
heap, and browsers open several connections at once. At 4 KiB a connection's
buffers take about 9 KiB, and 5 connections peak at about 44 KiB of heap. At
16 KiB they would take about 33 KiB per connection, and about 170 KiB for 5
connections, which is more RAM than most of the supported boards have spare.
Clients mostly send small requests, so the 4 KiB limit is only noticeable
when installing a certificate.

## Shell commands

| Command | |
|---|---|
| `bmc cert` | Show the certificate, its fingerprint and public key pin |
| `bmc cert names [<name>... \| clear]` | Show or set extra names |
| `bmc cert self-signed` | Replace an installed certificate with a self-signed one |
| `bmc cert regenerate` | Delete the key and certificate and reboot to generate new ones |
