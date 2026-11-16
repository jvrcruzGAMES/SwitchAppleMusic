from dataclasses import dataclass
import os
from pathlib import Path

@dataclass(frozen=True)
class Settings:
    device_token: str
    team_id: str
    key_id: str
    private_key: Path
    user_token_file: Path | None = None
    wrapper_url: str = "http://wrapper:80"
    decrypt_host: str = "wrapper"
    decrypt_port: int = 10020
    storefront: str = "us"
    musixmatch_key: str = ""
    cache_dir: Path = Path("/tmp/switchmusic-cache")
    max_cache_bytes: int = 512 * 1024 * 1024
    max_track_bytes: int = 64 * 1024 * 1024
    cache_ttl: int = 1800

    @classmethod
    def from_env(cls):
        token = os.environ.get("DEVICE_TOKEN", "")
        if len(token) < 32:
            raise ValueError("DEVICE_TOKEN must contain at least 32 characters")
        return cls(device_token=token, team_id=os.environ["APPLE_TEAM_ID"],
                   key_id=os.environ["APPLE_KEY_ID"],
                   private_key=Path(os.environ.get("APPLE_PRIVATE_KEY", "/run/secrets/apple_key")),
                   user_token_file=Path(os.environ.get("APPLE_MUSIC_USER_TOKEN_FILE", "/run/secrets/music_user_token")),
                   wrapper_url=os.environ.get("WRAPPER_URL", "http://wrapper:80"),
                   decrypt_host=os.environ.get("WRAPPER_DECRYPT_HOST", "wrapper"),
                   decrypt_port=int(os.environ.get("WRAPPER_DECRYPT_PORT", "10020")),
                   storefront=os.environ.get("APPLE_STOREFRONT", "us"),
                   musixmatch_key=os.environ.get("MUSIXMATCH_API_KEY", ""),
                   cache_dir=Path(os.environ.get("CACHE_DIR", "/tmp/switchmusic-cache")))
