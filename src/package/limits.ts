export interface PackageLimits {
  /** Maximum size of the .xsn file itself. */
  maxPackageBytes: number;
  /** Maximum number of entries in the package. */
  maxEntries: number;
  /** Maximum uncompressed size of a single entry. */
  maxEntryBytes: number;
  /** Maximum total uncompressed size of all entries. */
  maxTotalBytes: number;
}

export const DEFAULT_LIMITS: PackageLimits = {
  maxPackageBytes: 256 * 1024 * 1024,
  maxEntries: 10_000,
  maxEntryBytes: 128 * 1024 * 1024,
  maxTotalBytes: 512 * 1024 * 1024,
};
