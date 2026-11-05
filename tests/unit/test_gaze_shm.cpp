#include "test_framework.h"

#include "gaze/gaze_shm.h"

using namespace vrsfov;

VRSFOV_TEST(gaze_shm_roundtrip) {
  GazeShm channel{};
  channel.magic = kGazeMagic;
  channel.version = kGazeVersion;

  GazeShm sample{};
  sample.magic = kGazeMagic;
  sample.version = kGazeVersion;
  sample.monoNs = 123456;
  sample.u0 = 0.25f;
  sample.v0 = 0.30f;
  sample.u1 = 0.75f;
  sample.v1 = 0.70f;
  sample.valid = 1;
  sample.everValid = 1;
  sample.clientType = 2;
  gazeShmPublish(&channel, sample);

  GazeShm out{};
  CHECK(gazeShmRead(&channel, out));
  CHECK_EQ(out.monoNs, 123456);
  CHECK_NEAR(out.u0, 0.25, 1e-6);
  CHECK_NEAR(out.v1, 0.70, 1e-6);
  CHECK_EQ(out.valid, 1u);
}

VRSFOV_TEST(gaze_shm_rejects_bad_record) {
  GazeShm channel{};
  channel.magic = kGazeMagic;
  channel.version = kGazeVersion + 1;  // wrong protocol version
  GazeShm out{};
  CHECK(!gazeShmRead(&channel, out));

  GazeShm empty{};
  CHECK(!gazeShmRead(&empty, out));
  CHECK(!gazeShmRead(nullptr, out));
}
