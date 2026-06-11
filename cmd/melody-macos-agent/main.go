//go:build darwin

package main

import (
	"fmt"
	"os"

	"github.com/carnager/melody/internal/agent"
)

func main() {
	if err := agent.Run(agent.Options{
		ProgramName:       "melody-macos-agent",
		ConfigFileName:    "melody-macos-agent.toml",
		DefaultNameSuffix: "-macos",
	}); err != nil {
		fmt.Fprintf(os.Stderr, "melody-macos-agent: %v\n", err)
		os.Exit(1)
	}
}
