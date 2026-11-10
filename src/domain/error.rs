//! Stable error codes for the rejections Sedici returns to the game. A code is part of the log
//! contract: it never changes meaning and is never reused. The FFX return value stays what
//! AGENTS.md and the README define; the code says why inside that value.
use thiserror::Error;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u16)]
pub enum ErrorCode {
    Rejected = 0,
    StaleHandle = 1,
    NullDescriptor = 2,
    InvalidDescriptor = 3,
    Unsupported = 4,
    NamespaceExhausted = 5,
    Allocation = 6,
    WrapFailed = 7,
    NotRetired = 8,
    SlotBusy = 9,
    RetirementTimeout = 10,
}

impl ErrorCode {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Rejected => "E_REJECTED",
            Self::StaleHandle => "E_STALE_HANDLE",
            Self::NullDescriptor => "E_NULL_DESCRIPTOR",
            Self::InvalidDescriptor => "E_INVALID_DESCRIPTOR",
            Self::Unsupported => "E_UNSUPPORTED",
            Self::NamespaceExhausted => "E_NAMESPACE",
            Self::Allocation => "E_ALLOCATION",
            Self::WrapFailed => "E_WRAP_FAILED",
            Self::NotRetired => "E_NOT_RETIRED",
            Self::SlotBusy => "E_SLOT_BUSY",
            Self::RetirementTimeout => "E_RETIREMENT_TIMEOUT",
        }
    }

    /// Derives the code from a free-text reason. The first matching fragment wins; a reason that
    /// matches nothing is [`ErrorCode::Rejected`].
    pub fn classify(reason: &str) -> Self {
        const RULES: [(&str, ErrorCode); 13] = [
            ("destroy before GPU retirement", ErrorCode::NotRetired),
            ("retirement timed out", ErrorCode::RetirementTimeout),
            ("prepare slot", ErrorCode::SlotBusy),
            ("on a destroyed context", ErrorCode::StaleHandle),
            ("null descriptor", ErrorCode::NullDescriptor),
            ("namespace", ErrorCode::NamespaceExhausted),
            ("allocation failed", ErrorCode::Allocation),
            ("wrap failed", ErrorCode::WrapFailed),
            ("unsupported", ErrorCode::Unsupported),
            ("unimplemented", ErrorCode::Unsupported),
            ("names both", ErrorCode::InvalidDescriptor),
            ("without a", ErrorCode::InvalidDescriptor),
            ("empty display size", ErrorCode::InvalidDescriptor),
        ];
        RULES
            .iter()
            .find(|(fragment, _)| reason.contains(fragment))
            .map_or(Self::Rejected, |&(_, code)| code)
    }
}

/// A rejection with its stable code. The text is what the log prints after `rejected:`.
#[derive(Debug, Error, Clone, PartialEq, Eq)]
#[error("{reason} [code={}]", .code.name())]
pub struct Rejection {
    pub code: ErrorCode,
    pub reason: String,
}

impl Rejection {
    pub fn new(reason: impl Into<String>) -> Self {
        let reason = reason.into();
        Self {
            code: ErrorCode::classify(&reason),
            reason,
        }
    }

    pub fn with_code(code: ErrorCode, reason: impl Into<String>) -> Self {
        Self {
            code,
            reason: reason.into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rstest::rstest;

    #[rstest]
    #[case("destroy before GPU retirement", ErrorCode::NotRetired)]
    #[case("prepare slot is still in flight", ErrorCode::SlotBusy)]
    #[case("dispatch on a destroyed context", ErrorCode::StaleHandle)]
    #[case("unsupported format", ErrorCode::Unsupported)]
    #[case("something else entirely", ErrorCode::Rejected)]
    fn classifies_reasons(#[case] reason: &str, #[case] code: ErrorCode) {
        assert_eq!(ErrorCode::classify(reason), code);
    }

    #[test]
    fn rejection_display_carries_code() {
        assert_eq!(
            Rejection::new("null descriptor").to_string(),
            "null descriptor [code=E_NULL_DESCRIPTOR]"
        );
    }
}
