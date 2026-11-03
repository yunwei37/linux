// SPDX-License-Identifier: MIT
//! Allocation-free model ownership; the caller serializes core entry.
pub const SLOTS: usize = 8;
pub const PREFIX_TOKENS: usize = 64;
#[derive(Clone, Copy, Debug)]
#[repr(u32)]
pub enum PrefixOp {
    Reserve,
    Publish,
    Restore,
    Release,
}
#[derive(Clone, Copy, PartialEq, Eq)]
struct PrefixKey {
    scope: u64,
    tokens: [u32; PREFIX_TOKENS],
}
struct PrefixCache {
    enabled: bool,
    key: Option<PrefixKey>,
    hits: u64,
    publishes: u64,
}
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct PrefixStats {
    pub hits: u64,
    pub publishes: u64,
    pub skipped_tokens: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Invalid,
    Stale,
    Capacity,
    State,
    Engine,
    Quarantined,
}
pub type Result<T> = core::result::Result<T, Error>;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageState {
    Candidate,
    Validated,
    Available,
    Rejected,
    Retired,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InstanceState {
    Ready,
    Quiescing,
    Drained,
    Failed,
    Retired,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequestState {
    Admitted,
    Prefill,
    Decode,
    Completing,
    Done,
    Cancelling,
    Draining,
    Cancelled,
    Quarantined,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Phase {
    Prefill,
    Decode,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Policy {
    RoundRobin,
    Deadline,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    pub weights: [u8; 32],
    pub code: [u8; 32],
    pub layout: u64,
    pub platform: u64,
    pub adapter: u64,
}
#[derive(Clone, Copy, Debug)]
pub struct Envelope {
    pub host: u64,
    pub device: u64,
    pub weights_host: u64,
    pub weights_device: u64,
    pub request_host: u64,
    pub request_device: u64,
    pub max_prompt: u32,
    pub max_output: u32,
    pub max_sequence: u32,
    pub vocab: u32,
    pub max_batch: usize,
    pub prefill_quantum: u32,
}
pub struct Image {
    identity: Identity,
    bounds: Envelope,
    state: ImageState,
}
impl Image {
    pub fn candidate(identity: Identity, bounds: Envelope) -> Self {
        Self {
            identity,
            bounds,
            state: ImageState::Candidate,
        }
    }
    pub fn validate(&mut self, expected: Identity) -> Result<()> {
        if self.state != ImageState::Candidate {
            return Err(Error::State);
        }
        let b = self.bounds;
        if self.identity != expected
            || b.vocab == 0
            || b.max_prompt == 0
            || b.max_output == 0
            || b.max_sequence < b.max_prompt
            || b.prefill_quantum == 0
            || b.max_batch == 0
            || b.max_batch > SLOTS
            || b.weights_host > b.host
            || b.weights_device > b.device
        {
            self.state = ImageState::Rejected;
            return Err(Error::Invalid);
        }
        self.state = ImageState::Validated;
        Ok(())
    }
    pub fn state(&self) -> ImageState {
        self.state
    }
    pub fn identity(&self) -> Identity {
        self.identity
    }
    pub fn bounds(&self) -> Envelope {
        self.bounds
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Handle {
    pub slot: u32,
    pub generation: u32,
}
#[derive(Clone, Copy, Debug)]
pub struct Admission {
    pub session: u64,
    pub sharing_scope: u64,
    pub request: u32,
    pub output: u32,
    pub first_deadline: u64,
    pub token_interval: u64,
}
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct Work {
    pub handle: Handle,
    pub request: u32,
    pub position: u32,
    pub token: u32,
    pub count: u32,
}
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct Event {
    pub handle: Handle,
    pub request: u32,
    pub session: u64,
    pub token: u32,
    pub sequence: u32,
    pub terminal: bool,
    pub cancelled: bool,
}
/// Instance-private engine. run_batch returns only after device completion.
/// Failed reserve/load must roll back or quarantine inside the engine; uncertain
/// execution/release causes core to retain the reservation and stop admission.
pub trait Engine {
    fn load(&mut self, image: &Image) -> Result<()>;
    fn reserve(&mut self, work: Work, prompt: &[u32], output: u32) -> Result<()>;
    fn run_batch(&mut self, phase: Phase, work: &[Work], output: &mut [u32]) -> Result<()>;
    fn release(&mut self, work: Work) -> Result<()>;
    fn unload(&mut self) -> Result<()>;
    /// Fenced, typed KV transfer. Prefix storage is private to this Instance.
    /// Reserve must roll back on failure; uncertain copies are quarantined.
    fn prefix(&mut self, op: PrefixOp, work: Work) -> Result<()>;
}
#[derive(Clone, Copy)]
struct Request {
    prefix: Option<PrefixKey>,
    admission: Admission,
    handle: Handle,
    state: RequestState,
    prompt: u32,
    position: u32,
    token: u32,
    sequence: u32,
    deadline: u64,
}
impl Request {
    fn work(&self) -> Work {
        Work {
            handle: self.handle,
            request: self.admission.request,
            position: self.position,
            token: self.token,
            count: self.prompt,
        }
    }
}
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Accounting {
    pub live: u64,
    pub host: u64,
    pub device: u64,
    pub admitted: u64,
    pub released: u64,
    pub peak: u64,
    pub steps: u64,
}
/// One loaded image generation, owning the engine and all request resources.
pub struct Instance<E: Engine> {
    image: Image,
    engine: E,
    state: InstanceState,
    policy: Policy,
    requests: [Option<Request>; SLOTS],
    generations: [u32; SLOTS],
    cursor: usize,
    accounting: Accounting,
    prefix: Option<PrefixCache>,
}
impl<E: Engine> Instance<E> {
    pub fn load(mut image: Image, mut engine: E, policy: Policy) -> Result<Self> {
        if image.state != ImageState::Validated {
            return Err(Error::State);
        }
        engine.load(&image)?;
        image.state = ImageState::Available;
        let accounting = Accounting {
            host: image.bounds.weights_host,
            device: image.bounds.weights_device,
            ..Accounting::default()
        };
        Ok(Self {
            image,
            engine,
            state: InstanceState::Ready,
            policy,
            requests: [None; SLOTS],
            generations: [0; SLOTS],
            cursor: 0,
            accounting,
            prefix: None,
        })
    }
    /// A reserved cache slot is charged even for the disabled-policy control.
    /// This keeps the admission/capacity envelope identical in the ablation.
    pub fn configure_prefix(&mut self, enabled: bool) -> Result<()> {
        if self.state != InstanceState::Ready
            || self.accounting.live != 0
            || self.prefix.is_some()
            || self.image.bounds.prefill_quantum != PREFIX_TOKENS as u32
        {
            return Err(Error::State);
        }
        let b = self.image.bounds;
        let host = self
            .accounting
            .host
            .checked_add(b.request_host)
            .ok_or(Error::Capacity)?;
        let device = self
            .accounting
            .device
            .checked_add(b.request_device)
            .ok_or(Error::Capacity)?;
        if host > b.host || device > b.device {
            return Err(Error::Capacity);
        }
        self.engine.prefix(PrefixOp::Reserve, Self::cache_work())?;
        self.accounting.host = host;
        self.accounting.device = device;
        self.prefix = Some(PrefixCache {
            enabled,
            key: None,
            hits: 0,
            publishes: 0,
        });
        Ok(())
    }
    fn cache_work() -> Work {
        Work {
            handle: Handle {
                slot: (SLOTS - 1) as u32,
                generation: 1,
            },
            request: u32::MAX,
            position: 0,
            token: 0,
            count: PREFIX_TOKENS as u32,
        }
    }
    pub fn prefix_stats(&self) -> PrefixStats {
        match &self.prefix {
            Some(c) => PrefixStats {
                hits: c.hits,
                publishes: c.publishes,
                skipped_tokens: c.hits * PREFIX_TOKENS as u64,
            },
            None => PrefixStats::default(),
        }
    }
    pub fn state(&self) -> InstanceState {
        self.state
    }
    pub fn accounting(&self) -> Accounting {
        self.accounting
    }
    pub fn admit(&mut self, a: Admission, prompt: &[u32]) -> Result<Handle> {
        if self.state != InstanceState::Ready {
            return Err(Error::State);
        }
        let b = self.image.bounds;
        let n = u32::try_from(prompt.len()).map_err(|_| Error::Invalid)?;
        if a.session == 0
            || a.request == 0
            || n == 0
            || n > b.max_prompt
            || a.output == 0
            || a.output > b.max_output
            || n.checked_add(a.output)
                .filter(|n| *n <= b.max_sequence)
                .is_none()
            || prompt.iter().any(|t| *t >= b.vocab)
        {
            return Err(Error::Invalid);
        }
        if self
            .requests
            .iter()
            .flatten()
            .any(|r| r.admission.session == a.session && r.admission.request == a.request)
        {
            return Err(Error::Invalid);
        }
        let host = self
            .accounting
            .host
            .checked_add(b.request_host)
            .ok_or(Error::Capacity)?;
        let device = self
            .accounting
            .device
            .checked_add(b.request_device)
            .ok_or(Error::Capacity)?;
        if host > b.host || device > b.device {
            return Err(Error::Capacity);
        }
        let slot = (0..if self.prefix.is_some() {
            SLOTS - 1
        } else {
            SLOTS
        })
            .find(|i| self.requests[*i].is_none() && self.generations[*i] < u32::MAX)
            .ok_or(Error::Capacity)?;
        self.generations[slot] += 1; // never wrap a stale capability back into validity
        let handle = Handle {
            slot: slot as u32,
            generation: self.generations[slot],
        };
        let prefix = if a.sharing_scope != 0 && prompt.len() > PREFIX_TOKENS {
            let mut tokens = [0; PREFIX_TOKENS];
            tokens.copy_from_slice(&prompt[..PREFIX_TOKENS]);
            Some(PrefixKey {
                scope: a.sharing_scope,
                tokens,
            })
        } else {
            None
        };
        let r = Request {
            prefix,
            admission: a,
            handle,
            state: RequestState::Admitted,
            prompt: n,
            position: 0,
            token: 0,
            sequence: 0,
            deadline: a.first_deadline,
        };
        self.engine.reserve(r.work(), prompt, a.output)?;
        self.requests[slot] = Some(r);
        self.accounting.live += 1;
        self.accounting.admitted += 1;
        self.accounting.peak = self.accounting.peak.max(self.accounting.live);
        self.accounting.host = host;
        self.accounting.device = device;
        if prefix.is_some()
            && self
                .prefix
                .as_ref()
                .is_some_and(|c| c.enabled && c.key == prefix)
        {
            if self.engine.prefix(PrefixOp::Restore, r.work()).is_err() {
                self.requests[slot].as_mut().unwrap().state = RequestState::Quarantined;
                self.state = InstanceState::Failed;
                return Err(Error::Quarantined);
            }
            let r = self.requests[slot].as_mut().unwrap();
            r.position = PREFIX_TOKENS as u32;
            r.state = RequestState::Prefill;
            self.prefix.as_mut().unwrap().hits += 1;
        }
        Ok(handle)
    }
    fn request(&self, session: u64, h: Handle) -> Result<&Request> {
        let r = self
            .requests
            .get(h.slot as usize)
            .and_then(Option::as_ref)
            .ok_or(Error::Stale)?;
        if r.handle != h || r.admission.session != session {
            return Err(Error::Stale);
        }
        Ok(r)
    }
    pub fn request_state(&self, session: u64, h: Handle) -> Result<RequestState> {
        Ok(self.request(session, h)?.state)
    }
    pub fn cancel(&mut self, session: u64, h: Handle) -> Result<()> {
        let state = self.request(session, h)?.state;
        if state == RequestState::Quarantined {
            return Err(Error::Quarantined);
        }
        if matches!(state, RequestState::Done | RequestState::Cancelled) {
            return Ok(());
        }
        self.requests[h.slot as usize].as_mut().unwrap().state = RequestState::Cancelling;
        Ok(())
    }
    /// First acknowledge each output; after the last, separately retire the
    /// transport reference. No descriptor/arena reuse while a frame is pending.
    pub fn acknowledge(&mut self, session: u64, h: Handle, now: u64) -> Result<()> {
        let r = *self.request(session, h)?;
        if r.state == RequestState::Completing {
            let r = self.requests[h.slot as usize].as_mut().unwrap();
            r.state = if r.sequence == r.admission.output {
                RequestState::Done
            } else {
                RequestState::Decode
            };
            r.deadline = now.saturating_add(r.admission.token_interval);
            return Ok(());
        }
        if !matches!(r.state, RequestState::Done | RequestState::Cancelled) {
            return Err(Error::State);
        }
        if self.engine.release(r.work()).is_err() {
            self.requests[h.slot as usize].as_mut().unwrap().state = RequestState::Quarantined;
            self.state = InstanceState::Failed;
            return Err(Error::Quarantined);
        }
        self.requests[h.slot as usize] = None;
        self.accounting.live -= 1;
        self.accounting.released += 1;
        self.accounting.host -= self.image.bounds.request_host;
        self.accounting.device -= self.image.bounds.request_device;
        if self.state == InstanceState::Quiescing && self.accounting.live == 0 {
            self.state = InstanceState::Drained;
        }
        Ok(())
    }
    pub fn advance(&mut self, events: &mut [Option<Event>; SLOTS]) -> Result<usize> {
        events.fill(None);
        if !matches!(self.state, InstanceState::Ready | InstanceState::Quiescing) {
            return Err(Error::State);
        }
        for r in self.requests.iter_mut().flatten() {
            if r.state == RequestState::Cancelling {
                r.state = RequestState::Draining; // engine is synchronous and already fenced
                r.state = RequestState::Cancelled;
                events[0] = Some(Event {
                    handle: r.handle,
                    request: r.admission.request,
                    session: r.admission.session,
                    token: 0,
                    sequence: r.sequence,
                    terminal: true,
                    cancelled: true,
                });
                return Ok(1);
            }
        }
        let eligible = |r: &Request| {
            matches!(
                r.state,
                RequestState::Admitted | RequestState::Prefill | RequestState::Decode
            )
        };
        let mut first: Option<usize> = None;
        for offset in 0..SLOTS {
            let i = (self.cursor + offset) % SLOTS;
            if let Some(r) = self.requests[i].filter(eligible) {
                if first.is_none()
                    || (self.policy == Policy::Deadline
                        && r.deadline < self.requests[first.unwrap()].unwrap().deadline)
                {
                    first = Some(i);
                }
            }
        }
        let Some(first) = first else {
            return Ok(0);
        };
        let phase = if self.requests[first].unwrap().position < self.requests[first].unwrap().prompt
        {
            Phase::Prefill
        } else {
            Phase::Decode
        };
        let empty = Work {
            handle: Handle {
                slot: 0,
                generation: 0,
            },
            request: 0,
            position: 0,
            token: 0,
            count: 0,
        };
        let mut batch = [empty; SLOTS];
        let mut n = 0;
        for offset in 0..SLOTS {
            let i = (first + offset) % SLOTS;
            if let Some(r) = self.requests[i].filter(eligible) {
                let p = if r.position < r.prompt {
                    Phase::Prefill
                } else {
                    Phase::Decode
                };
                if p == phase && n < self.image.bounds.max_batch {
                    batch[n] = r.work();
                    n += 1;
                    self.requests[i].as_mut().unwrap().state = if p == Phase::Prefill {
                        RequestState::Prefill
                    } else {
                        RequestState::Decode
                    };
                }
            }
        }
        let mut output = [u32::MAX; SLOTS];
        if self
            .engine
            .run_batch(phase, &batch[..n], &mut output[..n])
            .is_err()
            || output[..n].iter().any(|t| *t >= self.image.bounds.vocab)
        {
            for w in &batch[..n] {
                self.requests[w.handle.slot as usize]
                    .as_mut()
                    .unwrap()
                    .state = RequestState::Quarantined;
            }
            self.state = InstanceState::Failed;
            return Err(Error::Quarantined);
        }
        let mut emitted = 0;
        for (j, w) in batch[..n].iter().enumerate() {
            let r = self.requests[w.handle.slot as usize].as_mut().unwrap();
            r.position = if phase == Phase::Prefill {
                r.position
                    .saturating_add(self.image.bounds.prefill_quantum)
                    .min(r.prompt)
            } else {
                r.position + 1
            };
            if phase == Phase::Prefill
                && r.position == PREFIX_TOKENS as u32
                && r.position < r.prompt
                && r.prefix.is_some()
                && self.prefix.as_ref().is_some_and(|c| c.enabled)
            {
                // Publish only a fenced, token-major prefix before decode can
                // transpose the arena. No request writes the cached extent.
                self.prefix.as_mut().unwrap().key = None;
                if self.engine.prefix(PrefixOp::Publish, *w).is_err() {
                    r.state = RequestState::Quarantined;
                    self.state = InstanceState::Failed;
                    return Err(Error::Quarantined);
                }
                let c = self.prefix.as_mut().unwrap();
                c.key = r.prefix;
                c.publishes += 1;
            }
            if r.position < r.prompt {
                r.state = RequestState::Prefill;
                continue;
            }
            r.sequence += 1;
            r.token = output[j];
            r.state = RequestState::Completing;
            events[emitted] = Some(Event {
                handle: r.handle,
                request: r.admission.request,
                session: r.admission.session,
                token: r.token,
                sequence: r.sequence,
                terminal: r.sequence == r.admission.output,
                cancelled: false,
            });
            emitted += 1;
        }
        self.cursor = (first + 1) % SLOTS;
        self.accounting.steps += n as u64;
        Ok(emitted)
    }
    pub fn quiesce(&mut self) -> Result<()> {
        if self.state != InstanceState::Ready {
            return Err(Error::State);
        }
        self.state = if self.accounting.live == 0 {
            InstanceState::Drained
        } else {
            InstanceState::Quiescing
        };
        Ok(())
    }
    pub fn retire(&mut self) -> Result<()> {
        if self.state != InstanceState::Drained {
            return Err(Error::State);
        }
        if self.prefix.is_some() {
            if self
                .engine
                .prefix(PrefixOp::Release, Self::cache_work())
                .is_err()
            {
                self.state = InstanceState::Failed;
                return Err(Error::Quarantined);
            }
            self.prefix = None;
            self.accounting.host -= self.image.bounds.request_host;
            self.accounting.device -= self.image.bounds.request_device;
        }
        if self.engine.unload().is_err() {
            self.state = InstanceState::Failed;
            return Err(Error::Quarantined);
        }
        self.state = InstanceState::Retired;
        self.image.state = ImageState::Retired;
        self.accounting.host = 0;
        self.accounting.device = 0;
        Ok(())
    }
}

#[cfg(test)]
#[path = "tests.rs"]
mod tests;
