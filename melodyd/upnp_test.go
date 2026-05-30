package main

import (
	"strings"
	"testing"
)

func TestSSDPHeader(t *testing.T) {
	resp := strings.Join([]string{
		"HTTP/1.1 200 OK",
		"CACHE-CONTROL: max-age=1800",
		"LOCATION: http://192.0.2.10:1400/xml/device_description.xml",
		"ST: urn:schemas-upnp-org:device:MediaRenderer:1",
		"",
	}, "\r\n")

	got := ssdpHeader(resp, "location")
	want := "http://192.0.2.10:1400/xml/device_description.xml"
	if got != want {
		t.Fatalf("ssdpHeader() = %q, want %q", got, want)
	}
}

func TestUPnPTimeRoundTrip(t *testing.T) {
	tests := map[string]float64{
		"00:00:00": 0,
		"00:02:03": 123,
		"01:02:03": 3723,
	}
	for in, want := range tests {
		if got := parseUPnPTime(in); got != want {
			t.Fatalf("parseUPnPTime(%q) = %v, want %v", in, got, want)
		}
		if got := secondsToUPnPTime(want); got != in {
			t.Fatalf("secondsToUPnPTime(%v) = %q, want %q", want, got, in)
		}
	}
}

func TestBuildSOAPBodyUsesServiceNamespaceAndEscapesArgs(t *testing.T) {
	body := buildSOAPBody(upnpAVTransportService, "SetAVTransportURI", map[string]string{
		"InstanceID": "0",
		"CurrentURI": "http://example.test/a?x=1&y=2",
	})

	if !strings.Contains(body, `xmlns:u="`+upnpAVTransportService+`"`) {
		t.Fatalf("SOAP body missing service namespace: %s", body)
	}
	if !strings.Contains(body, "http://example.test/a?x=1&amp;y=2") {
		t.Fatalf("SOAP body did not XML-escape argument: %s", body)
	}
}
