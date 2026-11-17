"""Exercise the pinned Gamdl adapter when playback dependencies are installed."""
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch
import pytest
from fastapi import HTTPException
pytest.importorskip('gamdl')
from gamdl.api.wrapper import WrapperApi
from gamdl.drm.models import DRMInfo
from gamdl.media import Song, SongCodec, AudioStream, StreamOrigin, WebPlaybackStream
from gamdl.media.song.stream import SongStreamResolver
from gamdl.media.stream import StreamTransport
from gamdl.stream import Streamer
from switchmusic.codecs import select_source, wrapper_source

@pytest.mark.parametrize('available,expected',[
    ([SongCodec.ALAC_LOSSLESS,SongCodec.AAC,SongCodec.AAC_WEB],SongCodec.ALAC_LOSSLESS),
    ([SongCodec.AAC,SongCodec.AAC_WEB],SongCodec.AAC),
    ([SongCodec.AAC_WEB],SongCodec.AAC_WEB),
])
async def test_real_gamdl_preference_fallback(available,expected):
    song=Song('123',api=SimpleNamespace())
    song.ensure_streamable=AsyncMock()
    streams=[AudioStream(codec=c,origin=StreamOrigin.WRAPPER_HLS,url='https://media.invalid/audio',transport=StreamTransport()) for c in available]
    async def hls(*,origin):
        return [s for s in streams if s.codec!=SongCodec.AAC_WEB] if origin==StreamOrigin.WRAPPER_HLS else []
    with patch.object(SongStreamResolver,'get_hls_streams',side_effect=hls),patch.object(SongStreamResolver,'get_web_streams',AsyncMock(return_value=[s for s in streams if s.codec==SongCodec.AAC_WEB])):
        assert (await select_source(song,SimpleNamespace())).codec==expected

@pytest.mark.parametrize('untyped',[False,True])
async def test_web_aac_uses_wrapper_with_actual_fairplay_signaling(untyped):
    source=WebPlaybackStream(codec=SongCodec.AAC_WEB,origin=StreamOrigin.WEBPLAYBACK,url='https://media.invalid/audio.m3u8',transport=StreamTransport(),media_id='123')
    uri='skd://itunes.apple.com/P123/s1/e1'
    info=DRMInfo(untyped_key_uri=uri) if untyped else DRMInfo(fairplay_streaming=uri)
    source.get_drm_info=AsyncMock(return_value=info)
    source.get_manifest=AsyncMock(return_value='manifest')
    adapted=await wrapper_source(source)
    assert (await adapted.get_drm_info()).fairplay_streaming==uri
    assert await adapted.get_manifest()=='manifest'
    assert adapted.codec==SongCodec.AAC_WEB and adapted.media_id=='123'
    wrapper=WrapperApi(SimpleNamespace(),'http://wrapper',me={},decrypt_host='wrapper',decrypt_port=10020)
    streamer=Streamer(adapted,wrapper_api=wrapper)
    assert streamer._wrapper==('wrapper',10020,'123')
    assert streamer._drm is None

async def test_web_aac_without_fairplay_fails_instead_of_falling_back_to_other_drm():
    source=SimpleNamespace(codec=SongCodec.AAC_WEB,get_drm_info=AsyncMock(return_value=DRMInfo(encryption_method='ISO-23001-7')))
    with pytest.raises(HTTPException) as error:
        await wrapper_source(source)
    assert error.value.status_code==502
    assert 'FairPlay' in error.value.detail

async def test_lyrics_use_actual_pinned_song_and_ttml_conversion():
    from switchmusic.lyrics import apple_lrc
    ttml='<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="00:01.250" end="00:03.000">Apple line</p><p begin="00:03.000" end="00:05.000">Next line</p></div></body></tt>'
    class Wrapper:
        me={'auth':{'dev_token':'SERVER_DEVELOPER_TOKEN','music_user_token':'SERVER_USER_TOKEN'}}
        async def __aenter__(self):return self
        async def __aexit__(self,*args):pass
    class Api:
        def __init__(self,transport,credentials,storefront,language):
            assert storefront=='us' and language=='en-US'
        async def __aenter__(self):return self
        async def __aexit__(self,*args):pass
        async def get_lyrics(self,song_id,**kwargs):
            assert song_id=='123'
            return {'data':[{'attributes':{'ttml':ttml}}]}
    settings=SimpleNamespace(wrapper_url='http://wrapper',decrypt_host='wrapper',decrypt_port=10020,storefront='us')
    with patch('gamdl.api.WrapperApi.create',AsyncMock(return_value=Wrapper())) as create,patch('gamdl.api.AppleMusicApi',Api),patch('gamdl.transport.ApiHttpClient',lambda *args:object()):
        result=await apple_lrc({'id':'123','type':'songs','attributes':{'hasLyrics':True}},settings)
        assert result=='[00:01.25]Apple line\n[00:03.00]Next line'
        create.assert_awaited_once_with(base_url='http://wrapper',decrypt_host='wrapper',decrypt_port=10020)
