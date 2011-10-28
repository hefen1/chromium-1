// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/compositor/layer_animator.h"

#include "base/debug/trace_event.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "ui/base/animation/animation_container.h"
#include "ui/gfx/compositor/compositor.h"
#include "ui/gfx/compositor/layer.h"
#include "ui/gfx/compositor/layer_animator_delegate.h"
#include "ui/gfx/compositor/layer_animation_sequence.h"

namespace ui {

class LayerAnimator;

namespace {

static const base::TimeDelta kDefaultTransitionDuration =
    base::TimeDelta::FromMilliseconds(200);

static const base::TimeDelta kTimerInterval =
    base::TimeDelta::FromMilliseconds(10);

} // namespace;

// LayerAnimator public --------------------------------------------------------

LayerAnimator::LayerAnimator(base::TimeDelta transition_duration)
    : delegate_(NULL),
      preemption_strategy_(IMMEDIATELY_SET_NEW_TARGET),
      transition_duration_(transition_duration),
      is_started_(false),
      disable_timer_for_test_(false) {
}

LayerAnimator::~LayerAnimator() {
  ClearAnimations();
}

// static
LayerAnimator* LayerAnimator::CreateDefaultAnimator() {
  return new LayerAnimator(base::TimeDelta::FromMilliseconds(0));
}

// static
LayerAnimator* LayerAnimator::CreateImplicitAnimator() {
  return new LayerAnimator(kDefaultTransitionDuration);
}

void LayerAnimator::SetTransform(const Transform& transform) {
  if (transition_duration_ == base::TimeDelta())
    delegate_->SetTransformFromAnimation(transform);
  else
    StartAnimationElement(LayerAnimationElement::CreateTransformElement(
        transform, transition_duration_));
}

Transform LayerAnimator::GetTargetTransform() const {
  LayerAnimationElement::TargetValue target;
  GetTargetValue(&target);
  return target.transform;
}

void LayerAnimator::SetBounds(const gfx::Rect& bounds) {
  if (transition_duration_ == base::TimeDelta())
    delegate_->SetBoundsFromAnimation(bounds);
  else
    StartAnimationElement(LayerAnimationElement::CreateBoundsElement(
        bounds, transition_duration_));
}

gfx::Rect LayerAnimator::GetTargetBounds() const {
  LayerAnimationElement::TargetValue target;
  GetTargetValue(&target);
  return target.bounds;
}

void LayerAnimator::SetOpacity(float opacity) {
  if (transition_duration_ == base::TimeDelta())
    delegate_->SetOpacityFromAnimation(opacity);
  else
    StartAnimationElement(LayerAnimationElement::CreateOpacityElement(
        opacity, transition_duration_));
}

float LayerAnimator::GetTargetOpacity() const {
  LayerAnimationElement::TargetValue target;
  GetTargetValue(&target);
  return target.opacity;
}

void LayerAnimator::SetDelegate(LayerAnimatorDelegate* delegate) {
  DCHECK(delegate);
  delegate_ = delegate;
}

void LayerAnimator::StartAnimation(LayerAnimationSequence* animation) {
  if (!StartSequenceImmediately(animation)) {
    // Attempt to preempt a running animation.
    switch (preemption_strategy_) {
      case IMMEDIATELY_SET_NEW_TARGET:
        ImmediatelySetNewTarget(animation);
        break;
      case IMMEDIATELY_ANIMATE_TO_NEW_TARGET:
        ImmediatelyAnimateToNewTarget(animation);
        break;
      case ENQUEUE_NEW_ANIMATION:
        EnqueueNewAnimation(animation);
        break;
      case REPLACE_QUEUED_ANIMATIONS:
        ReplaceQueuedAnimations(animation);
        break;
      case BLEND_WITH_CURRENT_ANIMATION: {
        // TODO(vollick) Add support for blended sequences and use them here.
        NOTIMPLEMENTED();
        break;
      }
    }
  }
  FinishAnyAnimationWithZeroDuration();
  UpdateAnimationState();
}

void LayerAnimator::ScheduleAnimation(LayerAnimationSequence* animation) {
  if (is_animating()) {
    animation_queue_.push_back(make_linked_ptr(animation));
    ProcessQueue();
  } else {
    StartSequenceImmediately(animation);
  }
  UpdateAnimationState();
}

void LayerAnimator::ScheduleTogether(
    const std::vector<LayerAnimationSequence*>& animations) {
  // Collect all the affected properties.
  LayerAnimationElement::AnimatableProperties animated_properties;
  std::vector<LayerAnimationSequence*>::const_iterator iter;
  for (iter = animations.begin(); iter != animations.end(); ++iter) {
    animated_properties.insert((*iter)->properties().begin(),
                               (*iter)->properties().end());
  }

  // Scheduling a zero duration pause that affects all the animated properties
  // will prevent any of the sequences from animating until there are no
  // running animations that affect any of these properties.
  ScheduleAnimation(
      new LayerAnimationSequence(
          LayerAnimationElement::CreatePauseElement(animated_properties,
                                                    base::TimeDelta())));

  // These animations (provided they don't animate any common properties) will
  // now animate together if trivially scheduled.
  for (iter = animations.begin(); iter != animations.end(); ++iter) {
    ScheduleAnimation(*iter);
  }

  UpdateAnimationState();
}

void LayerAnimator::StartAnimationElement(LayerAnimationElement* animation) {
  StartAnimation(new LayerAnimationSequence(animation));
}

void LayerAnimator::ScheduleAnimationElement(LayerAnimationElement* animation) {
  ScheduleAnimation(new LayerAnimationSequence(animation));
}

void LayerAnimator::ScheduleElementsTogether(
    const std::vector<LayerAnimationElement*>& animations) {
  std::vector<LayerAnimationSequence*> sequences;
  for (size_t i = 0; i < animations.size(); ++i)
    sequences.push_back(new LayerAnimationSequence(animations[i]));
  ScheduleTogether(sequences);
}

void LayerAnimator::StopAnimatingProperty(
    LayerAnimationElement::AnimatableProperty property) {
  while (true) {
    RunningAnimation* running = GetRunningAnimation(property);
    if (!running)
      break;
    FinishAnimation(running->sequence);
  }
}

void LayerAnimator::StopAnimating() {
  while (is_animating())
    FinishAnimation(running_animations_[0].sequence);
}

LayerAnimator::ScopedSettings::ScopedSettings(LayerAnimator* animator)
    : animator_(animator),
      old_transition_duration_(animator->transition_duration_) {
  SetTransitionDuration(kDefaultTransitionDuration);
}

LayerAnimator::ScopedSettings::~ScopedSettings() {
  animator_->transition_duration_ = old_transition_duration_;
}

void LayerAnimator::ScopedSettings::SetTransitionDuration(
    base::TimeDelta duration) {
  animator_->transition_duration_ = duration;
}

// LayerAnimator private -------------------------------------------------------

void LayerAnimator::Step(base::TimeTicks now) {
  last_step_time_ = now;
  std::vector<LayerAnimationSequence*> to_finish;
  for (RunningAnimations::iterator iter = running_animations_.begin();
       iter != running_animations_.end(); ++iter) {
    base::TimeDelta delta = now - (*iter).start_time;
    if (delta >= (*iter).sequence->duration() &&
        !(*iter).sequence->is_cyclic()) {
      to_finish.push_back((*iter).sequence);
    } else {
      (*iter).sequence->Progress(delta, delegate());
    }
  }
  for (std::vector<LayerAnimationSequence*>::iterator iter = to_finish.begin();
       iter != to_finish.end(); ++iter) {
    FinishAnimation(*iter);
  }
}

void LayerAnimator::SetStartTime(base::TimeTicks start_time) {
}

base::TimeDelta LayerAnimator::GetTimerInterval() const {
  return kTimerInterval;
}

void LayerAnimator::UpdateAnimationState() {
  if (disable_timer_for_test_)
    return;

  static ui::AnimationContainer* container = NULL;
  if (!container) {
    container = new AnimationContainer();
    container->AddRef();
  }

  const bool should_start = is_animating();
  if (should_start && !is_started_)
    container->Start(this);
  else if (!should_start && is_started_)
    container->Stop(this);

  is_started_ = should_start;
}

void LayerAnimator::RemoveAnimation(LayerAnimationSequence* sequence) {
    // First remove from running animations
  for (RunningAnimations::iterator iter = running_animations_.begin();
       iter != running_animations_.end(); ++iter) {
    if ((*iter).sequence == sequence) {
      running_animations_.erase(iter);
      break;
    }
  }

  // Then remove from the queue
  for (AnimationQueue::iterator queue_iter = animation_queue_.begin();
       queue_iter != animation_queue_.end(); ++queue_iter) {
    if ((*queue_iter) == sequence) {
      animation_queue_.erase(queue_iter);
      break;
    }
  }
}

void LayerAnimator::FinishAnimation(LayerAnimationSequence* sequence) {
  sequence->Progress(sequence->duration(), delegate());
  delegate()->OnLayerAnimationEnded(sequence);
  RemoveAnimation(sequence);
  ProcessQueue();
  UpdateAnimationState();
}

void LayerAnimator::FinishAnyAnimationWithZeroDuration() {
  // Special case: if we've started a 0 duration animation, just finish it now
  // and get rid of it. Note at each iteration of the loop, we either increment
  // i or remove an element from running_animations_, so
  // running_animations_.size() - i is always decreasing and we are always
  // progressing towards the termination of the loop.
  for (size_t i = 0; i < running_animations_.size();) {
    if (running_animations_[i].sequence->duration() == base::TimeDelta()) {
      running_animations_[i].sequence->Progress(
          running_animations_[i].sequence->duration(), delegate());
      delegate()->OnLayerAnimationEnded(running_animations_[i].sequence);
      RemoveAnimation(running_animations_[i].sequence);
    } else {
      ++i;
    }
  }
  ProcessQueue();
  UpdateAnimationState();
}

void LayerAnimator::ClearAnimations() {
  for (RunningAnimations::iterator iter = running_animations_.begin();
       iter != running_animations_.end(); ++iter) {
    (*iter).sequence->Abort();
  }
  running_animations_.clear();
  animation_queue_.clear();
  UpdateAnimationState();
}

LayerAnimator::RunningAnimation* LayerAnimator::GetRunningAnimation(
    LayerAnimationElement::AnimatableProperty property) {
  for (RunningAnimations::iterator iter = running_animations_.begin();
       iter != running_animations_.end(); ++iter) {
    if ((*iter).sequence->properties().find(property) !=
        (*iter).sequence->properties().end())
      return &(*iter);
  }
  return NULL;
}

void LayerAnimator::AddToQueueIfNotPresent(LayerAnimationSequence* animation) {
  // If we don't have the animation in the queue yet, add it.
  bool found_sequence = false;
  for (AnimationQueue::iterator queue_iter = animation_queue_.begin();
       queue_iter != animation_queue_.end(); ++queue_iter) {
    if ((*queue_iter) == animation) {
      found_sequence = true;
      break;
    }
  }

  if (!found_sequence)
    animation_queue_.push_front(make_linked_ptr(animation));
}

void LayerAnimator::RemoveAllAnimationsWithACommonProperty(
    LayerAnimationSequence* sequence,
    bool abort) {
  // For all the running animations, if they animate the same property,
  // progress them to the end and remove them. Note: at each iteration i is
  // incremented or an element is removed from the queue, so
  // animation_queue_.size() - i is always decreasing and we are always making
  // progress towards the loop terminating.
  for (size_t i = 0; i < running_animations_.size();) {
    if (running_animations_[i].sequence->HasCommonProperty(
            sequence->properties())) {
      // Finish the animation.
      if (abort){
        running_animations_[i].sequence->Abort();
      } else {
        running_animations_[i].sequence->Progress(
            running_animations_[i].sequence->duration(), delegate());
        delegate()->OnLayerAnimationEnded(running_animations_[i].sequence);
      }
      RemoveAnimation(running_animations_[i].sequence);
    } else {
      ++i;
    }
  }

  // Same for the queued animations. See comment above about loop termination.
  for (size_t i = 0; i < animation_queue_.size();) {
    if (animation_queue_[i]->HasCommonProperty(sequence->properties())) {
      // Finish the animation.
      if (abort)
        animation_queue_[i]->Abort();
      else
        animation_queue_[i]->Progress(animation_queue_[i]->duration(),
                                      delegate());
      RemoveAnimation(animation_queue_[i].get());
    } else {
      ++i;
    }
  }
}

void LayerAnimator::ImmediatelySetNewTarget(LayerAnimationSequence* sequence) {
  const bool abort = false;
  RemoveAllAnimationsWithACommonProperty(sequence, abort);
  sequence->Progress(sequence->duration(), delegate());
  delegate()->OnLayerAnimationEnded(sequence);
  RemoveAnimation(sequence);
}

void LayerAnimator::ImmediatelyAnimateToNewTarget(
    LayerAnimationSequence* sequence) {
  const bool abort = true;
  RemoveAllAnimationsWithACommonProperty(sequence, abort);
  AddToQueueIfNotPresent(sequence);
  StartSequenceImmediately(sequence);
}

void LayerAnimator::EnqueueNewAnimation(LayerAnimationSequence* sequence) {
  // It is assumed that if there was no conflicting animation, we would
  // not have been called. No need to check for a collision; just
  // add to the queue.
  animation_queue_.push_back(make_linked_ptr(sequence));
  ProcessQueue();
}

void LayerAnimator::ReplaceQueuedAnimations(LayerAnimationSequence* sequence) {
  // Remove all animations that aren't running. Note: at each iteration i is
  // incremented or an element is removed from the queue, so
  // animation_queue_.size() - i is always decreasing and we are always making
  // progress towards the loop terminating.
  for (size_t i = 0; i < animation_queue_.size();) {
    bool is_running = false;
    for (RunningAnimations::const_iterator iter = running_animations_.begin();
         iter != running_animations_.end(); ++iter) {
      if ((*iter).sequence == animation_queue_[i]) {
        is_running = true;
        break;
      }
    }
    if (!is_running)
      RemoveAnimation(animation_queue_[i].get());
    else
      ++i;
  }
  animation_queue_.push_back(make_linked_ptr(sequence));
  ProcessQueue();
}

void LayerAnimator::ProcessQueue() {
  bool started_sequence = false;
  do {
    started_sequence = false;
    // Build a list of all currently animated properties.
    LayerAnimationElement::AnimatableProperties animated;
    for (RunningAnimations::const_iterator iter = running_animations_.begin();
         iter != running_animations_.end(); ++iter) {
      animated.insert((*iter).sequence->properties().begin(),
                      (*iter).sequence->properties().end());
    }

    // Try to find an animation that doesn't conflict with an animated
    // property or a property that will be animated before it.
    for (AnimationQueue::iterator queue_iter = animation_queue_.begin();
         queue_iter != animation_queue_.end(); ++queue_iter) {
      if (!(*queue_iter)->HasCommonProperty(animated)) {
        StartSequenceImmediately((*queue_iter).get());
        started_sequence = true;
        break;
      }

      // Animation couldn't be started. Add its properties to the collection so
      // that we don't start a conflicting animation. For example, if our queue
      // has the elements { {T,B}, {B} } (that is, an element that animates both
      // the transform and the bounds followed by an element that animates the
      // bounds), and we're currently animating the transform, we can't start
      // the first element because it animates the transform, too. We cannot
      // start the second element, either, because the first element animates
      // bounds too, and needs to go first.
      animated.insert((*queue_iter)->properties().begin(),
                      (*queue_iter)->properties().end());
    }

    // If we started a sequence, try again. We may be able to start several.
  } while (started_sequence);
}

bool LayerAnimator::StartSequenceImmediately(LayerAnimationSequence* sequence) {
  // Ensure that no one is animating one of the sequence's properties already.
  for (RunningAnimations::const_iterator iter = running_animations_.begin();
       iter != running_animations_.end(); ++iter) {
    if ((*iter).sequence->HasCommonProperty(sequence->properties()))
      return false;
  }

  // All clear, actually start the sequence. Note: base::TimeTicks::Now has
  // a resolution that can be as bad as 15ms. If this causes glitches in the
  // animations, this can be switched to HighResNow() (animation uses Now()
  // internally).
  base::TimeTicks start_time = is_animating()
      ? last_step_time_
      : base::TimeTicks::Now();

  running_animations_.push_back(RunningAnimation(sequence, start_time));

  // Need to keep a reference to the animation.
  AddToQueueIfNotPresent(sequence);

  // Ensure that animations get stepped at their start time.
  Step(start_time);

  return true;
}

void LayerAnimator::GetTargetValue(
    LayerAnimationElement::TargetValue* target) const {
  for (RunningAnimations::const_iterator iter = running_animations_.begin();
       iter != running_animations_.end(); ++iter) {
    (*iter).sequence->GetTargetValue(target);
  }
}

}  // namespace ui
