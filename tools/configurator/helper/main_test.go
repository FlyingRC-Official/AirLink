package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

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
