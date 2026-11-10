//! Bookkeeping for a caller-owned reserved address range. Values are opaque: they are never
//! dereferenced. The caller serialises `issue`/`contains_issued` and keeps the range reserved for
//! at least as long as any token can be presented.
#[derive(Debug, Default)]
pub struct TokenNamespace {
    base: usize,
    stride: usize,
    capacity: usize,
    issued: usize,
}

impl TokenNamespace {
    pub fn new(base: usize, bytes: usize, stride: usize) -> Self {
        if base == 0 || stride == 0 || bytes < stride || bytes > usize::MAX - base {
            return Self::default();
        }
        Self {
            base,
            stride,
            capacity: bytes / stride,
            issued: 0,
        }
    }

    /// Next unused token, or `None` when unreserved or exhausted. Tokens are never reused.
    pub fn issue(&mut self) -> Option<usize> {
        if self.base == 0 || self.issued >= self.capacity {
            return None;
        }
        let token = self.base + self.stride * self.issued;
        self.issued += 1;
        Some(token)
    }

    pub fn contains_issued(&self, token: usize) -> bool {
        if self.base == 0 || token < self.base {
            return false;
        }
        let delta = token - self.base;
        delta.is_multiple_of(self.stride) && delta / self.stride < self.issued
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[test]
    fn issues_distinct_tokens_until_exhausted() {
        let mut ns = TokenNamespace::new(0x1000, 0x30, 0x10);
        let tokens: Vec<_> = std::iter::from_fn(|| ns.issue()).collect();
        assert_eq!(tokens, [0x1000, 0x1010, 0x1020]);
        assert!(ns.issue().is_none());
    }

    #[rstest]
    #[case(0x1000, true)]
    #[case(0x1010, false)] // not issued yet
    #[case(0x1001, false)] // misaligned
    #[case(0x0fff, false)] // below range
    fn recognises_only_issued(#[case] token: usize, #[case] expected: bool) {
        let mut ns = TokenNamespace::new(0x1000, 0x30, 0x10);
        ns.issue();
        assert_eq!(ns.contains_issued(token), expected);
    }

    #[test]
    fn invalid_reservation_issues_nothing() {
        assert!(TokenNamespace::new(0, 64, 8).issue().is_none());
        assert!(TokenNamespace::new(8, 4, 8).issue().is_none());
    }
}
