from dataclasses import dataclass
import os
from pathlib import Path

@dataclass(frozen=True)
class Settings:
    admin_password: str
    team_id: str
    key_id: str
    private_key: Path
    data_dir: Path = Path("/data")
    public_url: str = "http://localhost:8000"
    secure_cookies: bool = True
    wrapper_url: str = "http://wrapper:80"
    decrypt_host: str = "wrapper"
    decrypt_port: int = 10020
    storefront: str = "us"
    cache_dir: Path = Path("/tmp/switchmusic-cache")
    max_cache_bytes: int = 512 * 1024 * 1024
    max_track_bytes: int = 256 * 1024 * 1024
    cache_ttl: int = 1800

    @classmethod
    def from_env(cls):
        return cls(admin_password=os.environ.get("ADMIN_PASSWORD", ""), team_id=os.environ["APPLE_TEAM_ID"],
                   key_id=os.environ["APPLE_KEY_ID"],
                   private_key=Path(os.environ.get("APPLE_PRIVATE_KEY", "/run/secrets/apple_key")),
                   data_dir=Path(os.environ.get("DATA_DIR", "/data")),
                   public_url=os.environ.get("PUBLIC_URL", "http://localhost:8000").rstrip("/"),
                   secure_cookies=os.environ.get("SECURE_COOKIES", "true").lower()=="true",
                   wrapper_url=os.environ.get("WRAPPER_URL", "http://wrapper:80"),
                   decrypt_host=os.environ.get("WRAPPER_DECRYPT_HOST", "wrapper"),
                   decrypt_port=int(os.environ.get("WRAPPER_DECRYPT_PORT", "10020")),
                   storefront=os.environ.get("APPLE_STOREFRONT", "us"),
                   cache_dir=Path(os.environ.get("CACHE_DIR", "/tmp/switchmusic-cache")),
                   max_track_bytes=int(os.environ.get("MAX_TRACK_BYTES", str(256 * 1024 * 1024))),
                   max_cache_bytes=int(os.environ.get("MAX_CACHE_BYTES", str(512 * 1024 * 1024))))
