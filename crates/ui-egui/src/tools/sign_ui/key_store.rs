//! Digital IDs in air/OS's Key Store (`keystore_server`), as the Keychain and the Windows
//! certificate store keep them elsewhere: listed at each signing, with a `keystore:` reference.
//!
//! The Key Store holds data, not keys it signs with, and doesn't encrypt its database yet
//! (`Keyring::_EncryptToFlatBuffer` in Haiku's keystore_server is a TODO), so an ID goes in as
//! its PKCS #12, sealed with the ID's own password as a file would be: signing asks for the
//! password. Beside it the stored data has the certificate, so the list shows the ID without
//! the password. The Key Store asks the user before an application reads its keyring the first
//! time.

use pdfcraft_engine::sign::{self, Certificate};

/// The reference prefix of a Key Store ID.
pub(crate) const PREFIX: &str = "keystore:";

/// What the stored data starts with.
const MAGIC: &[u8] = b"PdfCraft digital ID 1\n";

/// Certificates are a few kilobytes; a longer length is a damaged entry.
const MAX_CERTIFICATE: usize = 1 << 20;

/// The `keystore:<SHA-256 of the certificate>` reference of an ID.
pub(crate) fn reference(c: &Certificate) -> String {
    let d = sign::keys::DigestAlg::Sha256.digest(&[&c.raw]);
    format!("{PREFIX}{}", d.iter().map(|b| format!("{b:02x}")).collect::<String>())
}

/// What goes in the Key Store for an ID: the certificate and its PKCS #12.
pub(crate) fn seal(certificate: &Certificate, p12: &[u8]) -> Vec<u8> {
    let len = u32::try_from(certificate.raw.len()).unwrap_or(u32::MAX);
    let mut out = Vec::with_capacity(MAGIC.len() + 4 + certificate.raw.len() + p12.len());
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(&certificate.raw);
    out.extend_from_slice(p12);
    out
}

/// The certificate and PKCS #12 of stored data; `None` for anything else.
pub(crate) fn open(data: &[u8]) -> Option<(Certificate, &[u8])> {
    let rest = data.strip_prefix(MAGIC)?;
    let (len, rest) = rest.split_first_chunk::<4>()?;
    let len = usize::try_from(u32::from_be_bytes(*len)).ok().filter(|&l| l <= MAX_CERTIFICATE)?;
    let (der, p12) = (rest.get(..len)?, rest.get(len..)?);
    Some((Certificate::parse(der).ok()?, p12))
}

/// The IDs in the Key Store: reference, name and certificate. Entries that aren't IDs are
/// skipped.
#[cfg(target_os = "haiku")]
pub(crate) fn identities() -> Result<Vec<(String, String, Certificate)>, String> {
    let stored = crate::native_services::key_store::list()?;
    Ok(stored
        .into_iter()
        .filter(|s| s.identifier.starts_with(PREFIX))
        .filter_map(|s| {
            let (certificate, _) = open(&s.data)?;
            Some((s.identifier, s.name, certificate))
        })
        .collect())
}

/// Keeps an ID (its certificate, and the PKCS #12 sealed with its password) in the Key Store
/// under `name`; its reference.
#[cfg(target_os = "haiku")]
pub(crate) fn add(certificate: &Certificate, name: &str, p12: &[u8]) -> Result<String, String> {
    let identifier = reference(certificate);
    let secret = crate::native_services::key_store::Stored { identifier: identifier.clone(), name: name.to_owned(), data: seal(certificate, p12) };
    crate::native_services::key_store::add(&secret).map_err(|e| format!("The digital ID couldn't be kept in the Key Store: {e}"))?;
    Ok(identifier)
}

/// The PKCS #12 of the ID with this reference.
#[cfg(target_os = "haiku")]
pub(crate) fn p12(reference: &str) -> Result<Vec<u8>, String> {
    let stored = crate::native_services::key_store::list()?;
    let s = stored.iter().find(|s| s.identifier == reference).ok_or_else(|| format!("{reference}: not in the Key Store"))?;
    open(&s.data).map(|(_, p12)| p12.to_vec()).ok_or_else(|| format!("{reference}: the Key Store entry is damaged"))
}

/// Deletes the ID with this reference from the Key Store.
#[cfg(target_os = "haiku")]
pub(crate) fn remove(reference: &str) -> Result<(), String> {
    crate::native_services::key_store::remove(reference)
}

#[cfg(test)]
mod tests {
    use pdfcraft_engine::sign::{Certificate, Name, PrivateKey, Time};

    use super::*;

    fn certificate() -> Certificate {
        let key = PrivateKey::generate_p256().unwrap_or_else(|e| panic!("{e}"));
        let dn = Name::build("Grace Hopper", "", "", "", "US");
        Certificate::self_signed(&dn, &key, Time::from_unix(1_700_000_000), 5, &[1, 2, 3]).unwrap_or_else(|e| panic!("{e}"))
    }

    #[test]
    fn sealed_ids_open_to_their_certificate_and_pkcs12() {
        let c = certificate();
        let sealed = seal(&c, b"pkcs12 bytes");
        let (opened, p12) = open(&sealed).unwrap_or_else(|| panic!("sealed data didn't open"));
        assert_eq!(opened.raw, c.raw);
        assert_eq!(p12, b"pkcs12 bytes");
        assert!(reference(&c).starts_with(PREFIX));
        assert_eq!(reference(&c).len(), PREFIX.len() + 64);
    }

    #[test]
    fn damaged_or_foreign_entries_dont_open() {
        let sealed = seal(&certificate(), b"p12");
        assert!(open(b"").is_none());
        assert!(open(b"a web password").is_none());
        assert!(open(&sealed[..MAGIC.len() + 2]).is_none(), "cut in the length");
        assert!(open(&sealed[..MAGIC.len() + 40]).is_none(), "cut in the certificate");
        let mut huge = MAGIC.to_vec();
        huge.extend_from_slice(&u32::MAX.to_be_bytes());
        assert!(open(&huge).is_none(), "a length past the data");
        let mut garbage = MAGIC.to_vec();
        garbage.extend_from_slice(&3u32.to_be_bytes());
        garbage.extend_from_slice(b"abc");
        assert!(open(&garbage).is_none(), "not a certificate");
    }
}
