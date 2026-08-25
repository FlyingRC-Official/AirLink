package main

import (
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (function roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return function(request)
}

func TestPrivateTargets(t *testing.T) {
	for _, value := range []string{"http://192.168.4.1", "http://10.0.0.2:80", "http://127.0.0.1:8080"} {
		if _, err := isPrivateTarget(value); err != nil {
			t.Fatalf("private target rejected: %s: %v", value, err)
		}
	}
	for _, value := range []string{"https://192.168.4.1", "http://8.8.8.8", "http://example.com", "http://10.0.0.2:9000"} {
		if _, err := isPrivateTarget(value); err == nil {
			t.Fatalf("unsafe target accepted: %s", value)
		}
	}
}

func TestSessionProtection(t *testing.T) {
	server := &helperServer{session: "test-session", client: &http.Client{Timeout: time.Second}}
	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodGet, "/helper/v1/health", nil)
	request.RemoteAddr = "127.0.0.1:12345"
	server.handler().ServeHTTP(recorder, request)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("expected unauthorized, got %d", recorder.Code)
	}
	recorder = httptest.NewRecorder()
	request = httptest.NewRequest(http.MethodGet, "/helper/v1/health?session=test-session", nil)
	request.RemoteAddr = "127.0.0.1:12345"
	server.handler().ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("expected ok, got %d", recorder.Code)
	}
}

func TestOtaProxyForwardsVerifiedMetadataAndFirmware(t *testing.T) {
	firmware := []byte{0xe9, 0x05, 0x02, 0x03}
	var captured *http.Request
	client := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		captured = request
		body, err := io.ReadAll(request.Body)
		if err != nil || !bytes.Equal(body, firmware) {
			t.Fatalf("upstream firmware mismatch: %x, %v", body, err)
		}
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     http.Header{"Content-Type": []string{"application/json"}},
			Body:       io.NopCloser(strings.NewReader(`{"ok":true}`)),
		}, nil
	})}
	server := &helperServer{session: "test-session", client: client, otaClient: client}
	request := httptest.NewRequest(http.MethodPost, "/helper/v1/ota", bytes.NewReader(firmware))
	request.RemoteAddr = "127.0.0.1:12345"
	request.SetBasicAuth("admin", "secret-password")
	request.Header.Set("X-AirLink-Session", "test-session")
	request.Header.Set("X-AirLink-Target", "http://127.0.0.1:8080")
	request.Header.Set("X-AirLink-Hardware", "airlink-c5-mesh-v1")
	request.Header.Set("X-AirLink-Flash-Bytes", "8388608")
	request.Header.Set("X-AirLink-PSRAM-Bytes", "8388608")
	request.Header.Set("X-AirLink-SHA256", strings.Repeat("a", 64))
	recorder := httptest.NewRecorder()
	server.handler().ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("expected ok, got %d: %s", recorder.Code, recorder.Body.String())
	}
	if captured == nil || captured.URL.Path != "/api/v1/ota" {
		t.Fatal("OTA was not sent to the allowlisted firmware endpoint")
	}
	if captured.Header.Get("X-AirLink-Hardware") != "airlink-c5-mesh-v1" {
		t.Fatal("hardware header not forwarded")
	}
	if captured.Header.Get("X-AirLink-SHA256") != strings.Repeat("a", 64) {
		t.Fatal("SHA-256 header not forwarded")
	}
	username, password, ok := captured.BasicAuth()
	if !ok || username != "admin" || password != "secret-password" {
		t.Fatal("device authorization not forwarded")
	}
}

func TestOtaProxyRejectsOversizedFirmware(t *testing.T) {
	server := &helperServer{session: "test-session", client: &http.Client{Timeout: time.Second}}
	request := httptest.NewRequest(http.MethodPost, "/helper/v1/ota", strings.NewReader("x"))
	request.ContentLength = maxOtaBody + 1
	request.RemoteAddr = "127.0.0.1:12345"
	request.Header.Set("X-AirLink-Session", "test-session")
	recorder := httptest.NewRecorder()
	server.handler().ServeHTTP(recorder, request)
	if recorder.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("expected 413, got %d", recorder.Code)
	}
}
