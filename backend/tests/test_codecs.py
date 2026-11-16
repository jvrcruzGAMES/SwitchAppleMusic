from enum import Enum
from types import SimpleNamespace, ModuleType
from unittest.mock import AsyncMock, patch
import sys
import pytest
from fastapi import HTTPException
from switchmusic.codecs import CODEC_PRIORITY, SUPPORTED_SONG_CODECS, select_source, decryption_allowed

class Codec(str, Enum):
    ALAC='alac'
    AAC='aac'
    AAC_WEB='aac-web'

@pytest.mark.parametrize('selected', ['alac-lossless','alac-hires','aac','aac-web'])
async def test_exact_preference_and_supported_result(selected):
    expected=SimpleNamespace(codec=SimpleNamespace(value=selected))
    song=SimpleNamespace(get_stream=AsyncMock(return_value=expected))
    media=ModuleType('gamdl.media');media.SongCodec=Codec
    with patch.dict(sys.modules,{'gamdl':ModuleType('gamdl'),'gamdl.media':media}):
        assert await select_source(song,'wrapper') is expected
    song.get_stream.assert_awaited_once_with(codec_priority=(Codec.ALAC,Codec.AAC,Codec.AAC_WEB),wrapper_api='wrapper')

async def test_atmos_is_rejected_even_if_upstream_returns_it():
    song=SimpleNamespace(get_stream=AsyncMock(return_value=SimpleNamespace(codec=SimpleNamespace(value='atmos'))))
    media=ModuleType('gamdl.media');media.SongCodec=Codec
    with patch.dict(sys.modules,{'gamdl':ModuleType('gamdl'),'gamdl.media':media}):
        with pytest.raises(HTTPException):await select_source(song,'wrapper')

@pytest.mark.parametrize('codec,method,expected',[
    ('alac-hires','wrapper',True),('alac-lossless','wrapper',True),
    ('aac','wrapper',True),('aac','fixed_prefetch_key',True),
    ('aac-web','widevine',True),('aac-web','none',True),
    ('aac-web','wrapper',False),('aac','none',False),('atmos','wrapper',False),('ac3','widevine',False),
])
def test_decryption_policy(codec,method,expected):
    assert decryption_allowed(codec,method)==expected

def test_atmos_and_ac3_not_in_policy():
    assert CODEC_PRIORITY==('alac','aac','aac-web')
    assert not {'atmos','ac3'} & SUPPORTED_SONG_CODECS
