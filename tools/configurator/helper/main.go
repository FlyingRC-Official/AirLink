// SPDX-License-Identifier: Apache-2.0
package main

import (
	"context"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strings"
	"time"
)

const (
	discoveryPort    = 49374
	discoveryRequest = "AIRLINK_DISCOVER_V1\n"
	maxProxyBody     = 2 << 20
	maxOtaBody       = 4 << 20
)

//go:embed static/index.html
var site embed.FS

type helperServer struct {
	session   string
	client    *http.Client
	otaClient *http.Client
}

type proxyRequest struct {
	Target   string          `json:"target"`
	Path     string          `json:"path"`
	Method   string          `json:"method"`
	Username string          `json:"username"`
	Password string          `json:"password"`
	Body     json.RawMessage `json:"body,omitempty"`
}

type discoveredDevice struct {
	Protocol   string `json:"protocol"`
	Product    string `json:"product"`
	HardwareID string `json:"hardware_id"`
	Serial     string `json:"serial"`
	Firmware   string `json:"firmware"`
	Role       string `json:"role"`
	HTTPPort   int    `json:"http_port"`
	IP         string `json:"ip"`
	Target     string `json:"target"`
}

var allowedProxy = map[string]map[string]bool{
	"/api/v1/status":                {"GET": true},
	"/api/v1/capabilities":          {"GET": true},
	"/api/v1/config":                {"GET": true, "PUT": true},
	"/api/v1/config/validate":       {"POST": true},
	"/api/v1/wifi/scan":             {"POST": true},
	"/api/v1/clients":               {"GET": true},
	"/api/v1/can":                   {"GET": true},
	"/api/v1/actions/reboot":        {"POST": true},
	"/api/v1/actions/factory-reset": {"POST": true},
}

func newSession() (string, error) {
	value := make([]byte, 24)
	if _, err := rand.Read(value); err != nil {
		return "", err
	}
	return hex.EncodeToString(value), nil
}

func isPrivateTarget(raw string) (*url.URL, error) {
	parsed, err := url.Parse(raw)
	if err != nil || parsed.Scheme != "http" || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return nil, errors.New("target must be a plain http URL")
	}
	ip := net.ParseIP(parsed.Hostname())
	if ip == nil || !(ip.IsPrivate() || ip.IsLoopback() || ip.IsLinkLocalUnicast()) {
		return nil, errors.New("target must be a private IP address")
	}
	if port := parsed.Port(); port != "" && port != "80" && port != "8080" {
		return nil, errors.New("target port is not allowed")
	}
	parsed.Path, parsed.RawPath = "", ""
	return parsed, nil
}

func (server *helperServer) authorized(request *http.Request) bool {
	host, _, err := net.SplitHostPort(request.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		return false
	}
	return request.Header.Get("X-AirLink-Session") == server.session || request.URL.Query().Get("session") == server.session
}

func writeJSON(response http.ResponseWriter, status int, value any) {
	response.Header().Set("Content-Type", "application/json")
	response.Header().Set("Cache-Control", "no-store")
	response.WriteHeader(status)
	_ = json.NewEncoder(response).Encode(value)
}

func (server *helperServer) health(response http.ResponseWriter, request *http.Request) {
	if !server.authorized(request) {
		writeJSON(response, http.StatusUnauthorized, map[string]string{"error": "invalid_helper_session"})
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"ok": true, "version": "0.3.2-dev", "discovery_port": discoveryPort})
}

func discover(ctx context.Context, timeout time.Duration) ([]discoveredDevice, error) {
	connection, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: 0})
	if err != nil {
		return nil, err
	}
	defer connection.Close()
	_ = connection.SetDeadline(time.Now().Add(timeout))
	if _, err = connection.WriteToUDP([]byte(discoveryRequest), &net.UDPAddr{IP: net.IPv4bcast, Port: discoveryPort}); err != nil {
		return nil, err
	}
	devices := map[string]discoveredDevice{}
	buffer := make([]byte, 513)
	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		default:
		}
		count, source, readErr := connection.ReadFromUDP(buffer)
		if readErr != nil {
			if timeoutError, ok := readErr.(net.Error); ok && timeoutError.Timeout() {
				break
			}
			return nil, readErr
		}
		if count == 0 || count > 512 {
			continue
		}
		var device discoveredDevice
		if json.Unmarshal(buffer[:count], &device) != nil || device.Protocol != "airlink-discovery/v1" || device.HardwareID == "" {
			continue
		}
		device.IP = source.IP.String()
		if device.HTTPPort == 0 {
			device.HTTPPort = 80
		}
		device.Target = fmt.Sprintf("http://%s:%d", device.IP, device.HTTPPort)
		key := device.Serial
		if key == "" {
			key = device.Target
		}
		devices[key] = device
	}
	result := make([]discoveredDevice, 0, len(devices))
	for _, device := range devices {
		result = append(result, device)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].Serial < result[j].Serial })
	return result, nil
}

func (server *helperServer) devices(response http.ResponseWriter, request *http.Request) {
	if !server.authorized(request) {
		writeJSON(response, http.StatusUnauthorized, map[string]string{"error": "invalid_helper_session"})
		return
	}
	timeout := 1200 * time.Millisecond
	if raw := request.URL.Query().Get("timeout_ms"); raw != "" {
		var value int
		if _, err := fmt.Sscanf(raw, "%d", &value); err == nil && value >= 100 && value <= 5000 {
			timeout = time.Duration(value) * time.Millisecond
		}
	}
	result, err := discover(request.Context(), timeout)
	if err != nil && !errors.Is(err, context.Canceled) {
		writeJSON(response, http.StatusServiceUnavailable, map[string]string{"error": "discovery_failed"})
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{"devices": result})
}

func (server *helperServer) proxy(response http.ResponseWriter, request *http.Request) {
	if !server.authorized(request) {
		writeJSON(response, http.StatusUnauthorized, map[string]string{"error": "invalid_helper_session"})
		return
	}
	request.Body = http.MaxBytesReader(response, request.Body, maxProxyBody)
	var input proxyRequest
	if json.NewDecoder(request.Body).Decode(&input) != nil {
		writeJSON(response, http.StatusBadRequest, map[string]string{"error": "invalid_proxy_request"})
		return
	}
	method := strings.ToUpper(input.Method)
	if !allowedProxy[input.Path][method] {
		writeJSON(response, http.StatusForbidden, map[string]string{"error": "proxy_path_not_allowed"})
		return
	}
	target, err := isPrivateTarget(input.Target)
	if err != nil {
		writeJSON(response, http.StatusForbidden, map[string]string{"error": err.Error()})
		return
	}
	target.Path = input.Path
	var body io.Reader
	if len(input.Body) > 0 && string(input.Body) != "null" {
		body = strings.NewReader(string(input.Body))
	}
	upstream, err := http.NewRequestWithContext(request.Context(), method, target.String(), body)
	if err != nil {
		writeJSON(response, http.StatusBadRequest, map[string]string{"error": "invalid_upstream_request"})
		return
	}
	upstream.Header.Set("Accept", "application/json")
	if body != nil {
		upstream.Header.Set("Content-Type", "application/json")
	}
	upstream.SetBasicAuth(input.Username, input.Password)
	result, err := server.client.Do(upstream)
	if err != nil {
		writeJSON(response, http.StatusBadGateway, map[string]string{"error": "device_unreachable"})
		return
	}
	defer result.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(result.Body, maxProxyBody+1))
	if err != nil || len(payload) > maxProxyBody {
		writeJSON(response, http.StatusBadGateway, map[string]string{"error": "invalid_device_response"})
		return
	}
	response.Header().Set("Content-Type", "application/json")
	response.Header().Set("Cache-Control", "no-store")
	response.WriteHeader(result.StatusCode)
	_, _ = response.Write(payload)
}

func (server *helperServer) ota(response http.ResponseWriter, request *http.Request) {
	if !server.authorized(request) {
		writeJSON(response, http.StatusUnauthorized, map[string]string{"error": "invalid_helper_session"})
		return
	}
	if request.Method != http.MethodPost {
		writeJSON(response, http.StatusMethodNotAllowed, map[string]string{"error": "method_not_allowed"})
		return
	}
	if request.ContentLength <= 0 || request.ContentLength > maxOtaBody {
		writeJSON(response, http.StatusRequestEntityTooLarge, map[string]string{"error": "invalid_ota_size"})
		return
	}
	target, err := isPrivateTarget(request.Header.Get("X-AirLink-Target"))
	if err != nil {
		writeJSON(response, http.StatusForbidden, map[string]string{"error": err.Error()})
		return
	}
	username, password, ok := request.BasicAuth()
	if !ok {
		writeJSON(response, http.StatusUnauthorized, map[string]string{"error": "missing_device_authorization"})
		return
	}
	requiredHeaders := []string{
		"X-AirLink-Hardware", "X-AirLink-Flash-Bytes", "X-AirLink-PSRAM-Bytes", "X-AirLink-SHA256",
	}
	for _, name := range requiredHeaders {
		if request.Header.Get(name) == "" {
			writeJSON(response, http.StatusBadRequest, map[string]string{"error": "missing_" + strings.ToLower(name)})
			return
		}
	}
	target.Path = "/api/v1/ota"
	request.Body = http.MaxBytesReader(response, request.Body, maxOtaBody)
	upstream, err := http.NewRequestWithContext(request.Context(), http.MethodPost, target.String(), request.Body)
	if err != nil {
		writeJSON(response, http.StatusBadRequest, map[string]string{"error": "invalid_upstream_request"})
		return
	}
	upstream.ContentLength = request.ContentLength
	upstream.Header.Set("Content-Type", "application/octet-stream")
	for _, name := range requiredHeaders {
		upstream.Header.Set(name, request.Header.Get(name))
	}
	upstream.SetBasicAuth(username, password)
	client := server.otaClient
	if client == nil {
		client = server.client
	}
	result, err := client.Do(upstream)
	if err != nil {
		writeJSON(response, http.StatusBadGateway, map[string]string{"error": "device_unreachable"})
		return
	}
	defer result.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(result.Body, maxProxyBody+1))
	if err != nil || len(payload) > maxProxyBody {
		writeJSON(response, http.StatusBadGateway, map[string]string{"error": "invalid_device_response"})
		return
	}
	response.Header().Set("Content-Type", "application/json")
	response.Header().Set("Cache-Control", "no-store")
	response.WriteHeader(result.StatusCode)
	_, _ = response.Write(payload)
}

func (server *helperServer) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/helper/v1/health", server.health)
	mux.HandleFunc("/helper/v1/devices", server.devices)
	mux.HandleFunc("/helper/v1/proxy", server.proxy)
	mux.HandleFunc("/helper/v1/ota", server.ota)
	mux.HandleFunc("/", func(response http.ResponseWriter, request *http.Request) {
		if !server.authorized(request) {
			http.Error(response, "invalid helper session", http.StatusUnauthorized)
			return
		}
		content, err := site.ReadFile("static/index.html")
		if err != nil {
			http.Error(response, "configurator missing", http.StatusInternalServerError)
			return
		}
		response.Header().Set("Content-Type", "text/html; charset=utf-8")
		response.Header().Set("Cache-Control", "no-store")
		_, _ = response.Write(content)
	})
	return http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		response.Header().Set("X-Content-Type-Options", "nosniff")
		response.Header().Set("Referrer-Policy", "no-referrer")
		response.Header().Set("Content-Security-Policy", "default-src 'self' 'unsafe-inline'; connect-src 'self' http: https:; img-src 'self' data:")
		mux.ServeHTTP(response, request)
	})
}

func openBrowser(address string) error {
	if os.Getenv("AIRLINK_CONFIGURATOR_DRY_RUN") == "1" {
		return nil
	}
	var command *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		command = exec.Command("rundll32", "url.dll,FileProtocolHandler", address)
	case "darwin":
		command = exec.Command("open", address)
	default:
		command = exec.Command("xdg-open", address)
	}
	return command.Start()
}

func main() {
	listen := flag.String("listen", "127.0.0.1:0", "loopback listen address")
	noBrowser := flag.Bool("no-browser", false, "do not open the browser")
	flag.Parse()
	address, err := net.ResolveTCPAddr("tcp", *listen)
	if err != nil || !address.IP.IsLoopback() {
		log.Fatal("helper must listen on a loopback address")
	}
	listener, err := net.ListenTCP("tcp", address)
	if err != nil {
		log.Fatal(err)
	}
	session, err := newSession()
	if err != nil {
		log.Fatal(err)
	}
	server := &helperServer{session: session, client: &http.Client{
		Timeout:       12 * time.Second,
		CheckRedirect: func(_ *http.Request, _ []*http.Request) error { return http.ErrUseLastResponse },
	}, otaClient: &http.Client{
		Timeout:       90 * time.Second,
		CheckRedirect: func(_ *http.Request, _ []*http.Request) error { return http.ErrUseLastResponse },
	}}
	launchURL := fmt.Sprintf("http://%s/?session=%s", listener.Addr().String(), session)
	fmt.Printf("AirLink Configurator V0.3.2-DEV: %s\n", launchURL)
	if !*noBrowser {
		_ = openBrowser(launchURL)
	}
	httpServer := &http.Server{Handler: server.handler(), ReadHeaderTimeout: 5 * time.Second}
	log.Fatal(httpServer.Serve(listener))
}
