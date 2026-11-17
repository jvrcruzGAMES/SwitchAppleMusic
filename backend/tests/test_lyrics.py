import asyncio
import threading
import pytest
from fastapi import HTTPException
from switchmusic.lyrics import lyrics, parse_lrc

SONG={'attributes':{'name':'Track','artistName':'Artist'}}

def test_lrc_timestamps_offsets_metadata_and_repeated_lines():
    lrc='[ar:Artist]\n[00:02.250][01:03.5]later\n[00:01.05]first\n[offset:-50]'
    assert parse_lrc(lrc)==[
        {'time_ms':1000,'text':'first'}, {'time_ms':2200,'text':'later'}, {'time_ms':63450,'text':'later'}]

def test_plain_lyrics_are_not_given_invented_timing():
    assert parse_lrc('plain text\n[ar:Artist]')==[]

def test_offset_clamps_and_enhanced_timestamps_are_removed():
    assert parse_lrc('[offset:-500]\n[00:00.1]<00:00.10>One <00:00.25>two')==[{'time_ms':0,'text':'One two'}]

@pytest.mark.parametrize('result',[None,'','Plain lyrics without timing'])
async def test_missing_synchronized_lyrics_is_404(monkeypatch,result):
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',lambda *args,**kwargs:result)
    with pytest.raises(HTTPException) as error:await lyrics(SONG)
    assert error.value.status_code==404

async def test_provider_errors_are_sanitized(monkeypatch):
    def fail(*args,**kwargs):raise RuntimeError('private upstream error')
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',fail)
    with pytest.raises(HTTPException) as error:await lyrics(SONG)
    assert error.value.status_code==502
    assert 'private' not in error.value.detail

async def test_blocking_search_runs_outside_api_thread(monkeypatch):
    api_thread=threading.get_ident()
    def search(*args,**kwargs):
        assert threading.get_ident()!=api_thread
        assert kwargs=={'synced_only':True}
        return '[00:01.000]line'
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',search)
    assert (await lyrics(SONG))['lines']==[{'time_ms':1000,'text':'line'}]

async def test_missing_title_does_not_call_provider(monkeypatch):
    def fail(*args,**kwargs):raise AssertionError('provider should not be called')
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',fail)
    with pytest.raises(HTTPException) as error:await lyrics({'attributes':{}})
    assert error.value.status_code==422

@pytest.mark.parametrize('primary',[None,'','Plain Apple lyrics','[ar:metadata]','[00:01.000] '])
async def test_apple_without_synced_lines_falls_back(monkeypatch,primary):
    calls=[]
    async def apple(*args):calls.append('apple');return primary
    def fallback(*args,**kwargs):calls.append('syncedlyrics');return '[00:02.000]Fallback'
    monkeypatch.setattr('switchmusic.lyrics.apple_lrc',apple)
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',fallback)
    result=await lyrics(SONG,object())
    assert calls==['apple','syncedlyrics'] and result['provider']=='syncedlyrics'

@pytest.mark.parametrize('failure',[ImportError('missing optional package'),RuntimeError('private credential error'),TimeoutError()])
async def test_apple_errors_fall_back_without_disclosing_details(monkeypatch,failure):
    async def apple(*args):raise failure
    monkeypatch.setattr('switchmusic.lyrics.apple_lrc',apple)
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',lambda *args,**kwargs:'[00:01]Fallback')
    result=await lyrics(SONG,object())
    assert result['provider']=='syncedlyrics' and 'private' not in str(result)

async def test_apple_cancellation_does_not_start_fallback(monkeypatch):
    async def apple(*args):raise asyncio.CancelledError()
    def forbidden(*args,**kwargs):raise AssertionError('Do not continue cancelled requests')
    monkeypatch.setattr('switchmusic.lyrics.apple_lrc',apple)
    monkeypatch.setattr('switchmusic.lyrics.syncedlyrics.search',forbidden)
    with pytest.raises(asyncio.CancelledError):await lyrics(SONG,object())
