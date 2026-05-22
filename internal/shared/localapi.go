package shared

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	LocalAPIConfigValue = "local"
	LocalAPIBaseURL     = "http://melodyd/api/v1"
)

func IsLocalAPIConfigValue(value string) bool {
	return strings.EqualFold(strings.TrimSpace(value), LocalAPIConfigValue)
}

func IsUnixAddress(value string) bool {
	value = strings.TrimSpace(value)
	return strings.Contains(value, "/") || strings.HasPrefix(value, ".")
}

func IsLoopbackTCPAddress(value string) bool {
	value = strings.TrimSpace(value)
	host, _, err := net.SplitHostPort(value)
	if err != nil {
		return false
	}
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func DefaultSocketPath() string {
	runtimeDir := Getenv("XDG_RUNTIME_DIR", filepath.Join(os.TempDir(), fmt.Sprintf("melody-%d", os.Getuid())))
	return filepath.Join(runtimeDir, "melody", "melodyd.sock")
}

func ResolveSocketPath(address string) string {
	address = strings.TrimSpace(address)
	if IsLocalAPIConfigValue(address) {
		return DefaultSocketPath()
	}
	return address
}

func APIBaseURLFromAddress(address string) (baseURL string, useLocalSocket bool, socketPath string, err error) {
	address = strings.TrimSpace(address)
	if address == "" {
		return "", false, "", fmt.Errorf("api.address is empty")
	}
	if IsLocalAPIConfigValue(address) {
		return LocalAPIBaseURL, true, ResolveSocketPath(address), nil
	}
	if IsUnixAddress(address) {
		return LocalAPIBaseURL, true, ResolveSocketPath(address), nil
	}
	return "http://" + address + "/api/v1", false, "", nil
}

func NewLocalHTTPClient(timeout time.Duration, socketPath string) *http.Client {
	transport := &http.Transport{
		DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "unix", socketPath)
		},
	}
	return &http.Client{
		Timeout:   timeout,
		Transport: transport,
	}
}
