#!/usr/bin/env python3
"""
ESP32-P4 FireRedVAD Configuration Module
Single source of truth for all connection settings and paths.

This module reads from config.ini in the project root and provides
typed access to all configuration values.

Usage:
    from esp32_config import ESP32Config
    
    config = ESP32Config()
    proto.connect(config.file_port, config.file_baud)
"""

import configparser
from pathlib import Path
from typing import Optional

class ESP32Config:
    """ESP32-P4 FireRedVAD configuration loader"""
    
    def __init__(self, config_path: Optional[Path] = None):
        """Load configuration from config.ini
        
        Args:
            config_path: Optional path to config.ini. If None, auto-detects from tools/ directory.
        """
        if config_path is None:
            # Config is in the same directory as this file (tools/)
            config_path = Path(__file__).parent / "config.ini"
        
        if not config_path.exists():
            raise FileNotFoundError(
                f"Configuration file not found: {config_path}\n"
                "Please ensure config.ini exists in the tools/ directory."
            )
        
        self.config_path = config_path
        self._config = configparser.ConfigParser()
        self._config.read(config_path)
    
    # ==================== Serial Configuration ====================
    
    @property
    def console_port(self) -> str:
        """Console/Debug UART port (UART0)"""
        return self._config.get('serial', 'console_port', fallback='COM4')
    
    @property
    def console_baud(self) -> int:
        """Console/Debug UART baud rate"""
        return self._config.getint('serial', 'console_baud', fallback=115200)
    
    @property
    def file_port(self) -> str:
        """File Transfer UART port (UART1)"""
        return self._config.get('serial', 'file_port', fallback='COM13')
    
    @property
    def file_baud(self) -> int:
        """File Transfer UART baud rate"""
        return self._config.getint('serial', 'file_baud', fallback=921600)
    
    # ============================================================================
    # Local Paths
    # ============================================================================
    
    @property
    def frvd_models_dir(self) -> Path:
        """Local converted models directory"""
        rel_path = self._config.get('local_paths', 'frvd_models', fallback='frvd_models')
        return (self.config_path.parent.parent / rel_path).resolve()
    
    @property
    def pth_models_dir(self) -> Path:
        """Get the path to original PyTorch models."""
        rel_path = self._config.get('local_paths', 'pth_models', fallback='../../pth_models')
        return (self.config_path.parent.parent / rel_path).resolve()
    
    # ==================== Helper Methods ====================
    
    def print_config(self):
        """Print current configuration for debugging"""
        print("=" * 70)
        print("ESP32-P4 VAD Configuration")
        print("=" * 70)
        print(f"Config file: {self.config_path}")
        print()
        print("Serial:")
        print(f"  Console:       {self.console_port} @ {self.console_baud} baud")
        print(f"  File Transfer: {self.file_port} @ {self.file_baud} baud")
        print()
        print("Local Paths:")
        print(f"  FRVD Models: {self.frvd_models_dir}")
        print(f"  PTH Models:  {self.pth_models_dir}")
        print("=" * 70)
        print()


# Global instance for convenience
_global_config: Optional[ESP32Config] = None

def get_config() -> ESP32Config:
    """Get global configuration instance (singleton pattern)"""
    global _global_config
    if _global_config is None:
        _global_config = ESP32Config()
    return _global_config


if __name__ == "__main__":
    # Test the configuration
    config = ESP32Config()
    config.print_config()
