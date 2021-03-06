// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/media/timeline/timeline_function.h"

#include <limits>
#include <utility>

#include "lib/fxl/logging.h"

namespace media {

// static
int64_t TimelineFunction::Apply(
    int64_t subject_time, int64_t reference_time,
    TimelineRate rate,  // subject_delta / reference_delta
    int64_t reference_input) {
  return rate.Scale(reference_input - reference_time) + subject_time;
}

// static
TimelineFunction TimelineFunction::Compose(const TimelineFunction& bc,
                                           const TimelineFunction& ab,
                                           bool exact) {
  // TODO(dalesat): Improve this implementation.
  // This particular approach to composing two timeline functions comprimises
  // range and accuracy (in some cases) for simplicity. It should be replaced
  // with something that provides maximum range and accuracy without adding a
  // lot of runtime cost.
  return TimelineFunction(bc.Apply(ab.subject_time()), ab.reference_time(),
                          TimelineRate::Product(ab.rate(), bc.rate(), exact));
}

TimelineFunction::TimelineFunction(
    const fuchsia::media::TimelineTransform& from)
    : subject_time_(from.subject_time),
      reference_time_(from.reference_time),
      rate_(TimelineRate(from.subject_delta, from.reference_delta)) {}

fuchsia::media::TimelineTransform TimelineFunction::ToTimelineTransform()
    const {
  fuchsia::media::TimelineTransform result;
  result.subject_time = subject_time();
  result.reference_time = reference_time();
  result.subject_delta = subject_delta();
  result.reference_delta = reference_delta();
  return result;
}

}  // namespace media
