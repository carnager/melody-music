package main

import (
	"fmt"
	"os"

	"github.com/carnager/melody/internal/agent"
)

func main() {
	if err := agent.Run(agent.Options{
		ProgramName:    "melody-agent",
		ConfigFileName: "melody-agent.toml",
	}); err != nil {
		fmt.Fprintf(os.Stderr, "melody-agent: %v\n", err)
		os.Exit(1)
	}
}
