"""Offline tests for DASH manifest parsing (SegmentTemplate/List/Base)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.dash import parse_manifest


TEMPLATE_TIMELINE = """<MPD mediaPresentationDuration="PT6S">
  <Period>
    <AdaptationSet mimeType="video/mp4">
      <SegmentTemplate media="$RepresentationID$-$Number%03d$.m4s"
                       initialization="$RepresentationID$-init.mp4" startNumber="1">
        <SegmentTimeline>
          <S t="0" d="2" r="2"/>
        </SegmentTimeline>
      </SegmentTemplate>
      <Representation id="v0" bandwidth="800000"/>
      <Representation id="v1" bandwidth="200000"/>
    </AdaptationSet>
  </Period>
</MPD>"""


def test_segment_template_timeline_and_variant_choice():
    s = parse_manifest(TEMPLATE_TIMELINE, "https://h/mpd/x.mpd", prefer="best")
    assert s.bandwidth == 800000                       # highest picked
    assert s.init_url == "https://h/mpd/v0-init.mp4"
    assert s.segment_urls == [
        "https://h/mpd/v0-001.m4s", "https://h/mpd/v0-002.m4s", "https://h/mpd/v0-003.m4s",
    ]
    worst = parse_manifest(TEMPLATE_TIMELINE, "https://h/mpd/x.mpd", prefer="worst")
    assert worst.bandwidth == 200000
    assert worst.init_url == "https://h/mpd/v1-init.mp4"


SEGMENT_LIST = """<MPD>
  <Period>
    <BaseURL>https://h/v/</BaseURL>
    <AdaptationSet mimeType="video/mp4">
      <Representation id="r0" bandwidth="500000">
        <SegmentList duration="2" timescale="1">
          <Initialization sourceURL="init.mp4"/>
          <SegmentURL media="seg1.m4s"/>
          <SegmentURL media="seg2.m4s"/>
          <SegmentURL media="seg3.m4s"/>
        </SegmentList>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>"""


def test_segment_list():
    s = parse_manifest(SEGMENT_LIST, "https://h/mpd/x.mpd")
    assert s.init_url == "https://h/v/init.mp4"
    assert s.segment_urls == [
        "https://h/v/seg1.m4s", "https://h/v/seg2.m4s", "https://h/v/seg3.m4s",
    ]


SEGMENT_BASE = """<MPD>
  <Period>
    <AdaptationSet mimeType="video/mp4">
      <Representation id="r0" bandwidth="900000">
        <BaseURL>video-900k.mp4</BaseURL>
        <SegmentBase indexRange="0-999">
          <Initialization range="0-833"/>
        </SegmentBase>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>"""


def test_segment_base_single_file():
    s = parse_manifest(SEGMENT_BASE, "https://h/mpd/x.mpd")
    # One self-contained file; init is embedded, so no separate init URL.
    assert s.init_url is None
    assert s.segment_urls == ["https://h/mpd/video-900k.mp4"]


BARE_BASEURL = """<MPD>
  <Period>
    <AdaptationSet mimeType="video/mp4">
      <Representation id="r0" bandwidth="100000">
        <BaseURL>https://cdn/clip.mp4</BaseURL>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>"""


def test_bare_baseurl_single_file():
    s = parse_manifest(BARE_BASEURL, "https://h/mpd/x.mpd")
    assert s.init_url is None
    assert s.segment_urls == ["https://cdn/clip.mp4"]


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all dash tests passed")
