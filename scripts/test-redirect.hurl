# SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# With CONFIG_APP_HTTP=n plain HTTP redirects to the same URL over HTTPS.
# Run without --location so the 308 itself is checked, eg:
#   hurl --variable host=192.168.1.110 --test scripts/test-redirect.hurl

# Web UI
GET http://{{host}}/
HTTP 308
[Asserts]
header "Location" == "https://{{host}}/"

# Redfish, the port in the Host header is dropped
GET http://{{host}}:80/redfish/v1/Managers
HTTP 308
[Asserts]
header "Location" == "https://{{host}}/redfish/v1/Managers"

# 308 not 301/302, so clients repeat the method and body
POST http://{{host}}/redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json
```
{ "ResetType": "Bogus" }
```
HTTP 308
[Asserts]
header "Location" == "https://{{host}}/redfish/v1/Systems/system/Actions/ComputerSystem.Reset"

# Following the redirect with the body gets the HTTPS answer
POST http://{{host}}/redfish/v1/Systems/system/Actions/ComputerSystem.Reset
Content-Type: application/json
[Options]
insecure: true
location-trusted: true
user: {{user}}
```
{ "ResetType": "Bogus" }
```
HTTP 400

# HEAD isn't redirected, the server would answer it without the callback
HEAD http://{{host}}/redfish/v1
HTTP 405
