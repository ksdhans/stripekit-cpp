#!/usr/bin/env bash
#
# tag-release.sh
#
# Validates release readiness and creates an annotated release tag.
# @author StripeKit contributors
# @License See LICENSE
#

set -euo pipefail

# Text formatting
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    COLOR_RESET="\033[0m"
    COLOR_RED="\033[31m"
    COLOR_GREEN="\033[32m"
    COLOR_YELLOW="\033[33m"
    COLOR_BLUE="\033[34m"
    COLOR_BOLD="\033[1m"
else
    COLOR_RESET=""
    COLOR_RED=""
    COLOR_GREEN=""
    COLOR_YELLOW=""
    COLOR_BLUE=""
    COLOR_BOLD=""
fi

log_ok() {
    echo -e "${COLOR_GREEN}[OK]${COLOR_RESET} $*"
}

log_info() {
    echo -e "${COLOR_BLUE}[INFO]${COLOR_RESET} $*"
}

log_warn() {
    echo -e "${COLOR_YELLOW}[WARN]${COLOR_RESET} $*"
}

log_fail() {
    echo -e "${COLOR_RED}[FAIL]${COLOR_RESET} $*"
}

show_help() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Checks version number, changelog, and git status, then creates an annotated release tag.

Options:
  -n, --dry-run             Perform all checks without creating or pushing the tag
  -p, --push                Push the tag to 'origin' after creating it
  -m, --message <msg>       Custom annotation message (default: "StripeKit <version>")
  -f, --force               Bypass non-fatal check errors and force tagging
      --no-remote           Skip remote fetch and synchronization checks
      --allow-branch <name> Allow tagging from a branch other than 'main'
      --allow-untracked     Allow untracked files in the working directory
  -h, --help                Show this help message and exit

Examples:
  $(basename "$0") --dry-run
  $(basename "$0")
  $(basename "$0") --push
  $(basename "$0") -m "StripeKit 1.0.0 Release" --push
EOF
}

# Default options
DRY_RUN=false
PUSH_TAG=false
CUSTOM_MESSAGE=""
FORCE=false
CHECK_REMOTE=true
EXPECTED_BRANCH="main"
ALLOW_UNTRACKED=false

# Parse command line options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -n|--dry-run)
            DRY_RUN=true
            shift
            ;;
        -p|--push)
            PUSH_TAG=true
            shift
            ;;
        -m|--message)
            if [[ $# -lt 2 ]]; then
                log_fail "Option '$1' requires an argument."
                exit 1
            fi
            CUSTOM_MESSAGE="$2"
            shift 2
            ;;
        -f|--force)
            FORCE=true
            shift
            ;;
        --no-remote)
            CHECK_REMOTE=false
            shift
            ;;
        --allow-branch)
            if [[ $# -lt 2 ]]; then
                log_fail "Option '$1' requires a branch name."
                exit 1
            fi
            EXPECTED_BRANCH="$2"
            shift 2
            ;;
        --allow-untracked)
            ALLOW_UNTRACKED=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            log_fail "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

ERRORS_FOUND=0

record_error() {
    log_fail "$*"
    ERRORS_FOUND=$((ERRORS_FOUND + 1))
}

# 1. Locate repository root
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    log_fail "Not inside a Git repository."
    exit 1
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

log_info "Running pre-release checks in: ${REPO_ROOT}"

# 2. Check current branch
CURRENT_BRANCH="$(git symbolic-ref --short HEAD 2>/dev/null || true)"
if [[ -z "$CURRENT_BRANCH" ]]; then
    if [[ "$FORCE" == true ]]; then
        log_warn "HEAD is detached (forced release mode)."
    else
        record_error "HEAD is detached. Releases must be tagged from the '${EXPECTED_BRANCH}' branch."
    fi
elif [[ "$CURRENT_BRANCH" != "$EXPECTED_BRANCH" ]]; then
    if [[ "$FORCE" == true ]]; then
        log_warn "Current branch is '${CURRENT_BRANCH}' instead of '${EXPECTED_BRANCH}' (forced release mode)."
    else
        record_error "Current branch is '${CURRENT_BRANCH}', but releases must be tagged from '${EXPECTED_BRANCH}'."
    fi
else
    log_ok "Current branch is '${CURRENT_BRANCH}'."
fi

# 3. Check for uncommitted changes
HAS_STAGED_CHANGES=false
HAS_UNSTAGED_CHANGES=false
HAS_UNTRACKED_FILES=false

if ! git diff --cached --quiet; then
    HAS_STAGED_CHANGES=true
fi

if ! git diff --quiet; then
    HAS_UNSTAGED_CHANGES=true
fi

UNTRACKED_FILES="$(git ls-files --others --exclude-standard)"
if [[ -n "$UNTRACKED_FILES" ]]; then
    HAS_UNTRACKED_FILES=true
fi

if [[ "$HAS_STAGED_CHANGES" == true || "$HAS_UNSTAGED_CHANGES" == true ]]; then
    if [[ "$FORCE" == true ]]; then
        log_warn "Working directory has uncommitted tracked changes (forced release mode)."
    else
        record_error "Working directory has uncommitted changes. All changes must be committed before tagging."
        if [[ "$HAS_STAGED_CHANGES" == true ]]; then
            echo -e "   ${COLOR_RED}Staged changes:${COLOR_RESET}"
            git diff --cached --name-status | sed 's/^/     /'
        fi
        if [[ "$HAS_UNSTAGED_CHANGES" == true ]]; then
            echo -e "   ${COLOR_RED}Unstaged changes:${COLOR_RESET}"
            git diff --name-status | sed 's/^/     /'
        fi
    fi
else
    log_ok "No uncommitted tracked changes."
fi

if [[ "$HAS_UNTRACKED_FILES" == true ]]; then
    if [[ "$ALLOW_UNTRACKED" == true || "$FORCE" == true ]]; then
        log_warn "Working directory contains untracked files (ignored)."
    else
        record_error "Working directory contains untracked files. Commit, stash, or clean them before tagging:"
        echo "$UNTRACKED_FILES" | head -n 10 | sed 's/^/     /'
        UNTRACKED_COUNT="$(echo "$UNTRACKED_FILES" | wc -l)"
        if [[ "$UNTRACKED_COUNT" -gt 10 ]]; then
            echo "     ... ($((UNTRACKED_COUNT - 10)) more)"
        fi
        echo "   (Use --allow-untracked to ignore untracked files)"
    fi
else
    log_ok "No untracked files present."
fi

# 4. Check remote synchronization
if [[ "$CHECK_REMOTE" == true ]]; then
    if git remote | grep -q "^origin$"; then
        log_info "Checking synchronization with remote 'origin'..."
        if git fetch origin "$EXPECTED_BRANCH" --tags --quiet 2>/dev/null; then
            LOCAL_SHA="$(git rev-parse HEAD)"
            if REMOTE_SHA="$(git rev-parse "origin/${EXPECTED_BRANCH}" 2>/dev/null)"; then
                AHEAD_COUNT="$(git rev-list --count "origin/${EXPECTED_BRANCH}..HEAD" 2>/dev/null || echo 0)"
                BEHIND_COUNT="$(git rev-list --count "HEAD..origin/${EXPECTED_BRANCH}" 2>/dev/null || echo 0)"

                if [[ "$AHEAD_COUNT" -gt 0 && "$BEHIND_COUNT" -gt 0 ]]; then
                    if [[ "$FORCE" == true ]]; then
                        log_warn "Local '${EXPECTED_BRANCH}' has diverged from 'origin/${EXPECTED_BRANCH}' (forced mode)."
                    else
                        record_error "Local branch '${EXPECTED_BRANCH}' has diverged from 'origin/${EXPECTED_BRANCH}' (ahead ${AHEAD_COUNT}, behind ${BEHIND_COUNT})."
                    fi
                elif [[ "$AHEAD_COUNT" -gt 0 ]]; then
                    if [[ "$FORCE" == true ]]; then
                        log_warn "Local '${EXPECTED_BRANCH}' is ahead of 'origin/${EXPECTED_BRANCH}' by ${AHEAD_COUNT} commit(s) (forced mode)."
                    else
                        record_error "Local branch '${EXPECTED_BRANCH}' has ${AHEAD_COUNT} unpushed commit(s). Push your changes to 'origin/${EXPECTED_BRANCH}' and verify CI passes before tagging."
                    fi
                elif [[ "$BEHIND_COUNT" -gt 0 ]]; then
                    if [[ "$FORCE" == true ]]; then
                        log_warn "Local '${EXPECTED_BRANCH}' is behind 'origin/${EXPECTED_BRANCH}' by ${BEHIND_COUNT} commit(s) (forced mode)."
                    else
                        record_error "Local branch '${EXPECTED_BRANCH}' is behind 'origin/${EXPECTED_BRANCH}' by ${BEHIND_COUNT} commit(s). Pull latest commits before tagging."
                    fi
                else
                    log_ok "Local '${EXPECTED_BRANCH}' is in sync with 'origin/${EXPECTED_BRANCH}' (${LOCAL_SHA:0:8})."
                fi
            else
                log_warn "Remote branch 'origin/${EXPECTED_BRANCH}' does not exist yet."
            fi
        else
            log_warn "Could not connect to remote 'origin' to verify branch status. (Use --no-remote to skip)."
        fi
    else
        log_info "No remote named 'origin' configured; skipping remote sync check."
    fi
else
    log_info "Skipping remote checks (--no-remote)."
fi

# 5. Extract and validate project version from CMakeLists.txt
CMAKE_FILE="$REPO_ROOT/CMakeLists.txt"
if [[ ! -f "$CMAKE_FILE" ]]; then
    record_error "CMakeLists.txt not found at repository root."
    PROJECT_VERSION=""
else
    PROJECT_VERSION="$(awk '
        /project[[:space:]]*\(/,/\)/ {
            for (i = 1; i <= NF; i++) {
                if ($i == "VERSION") {
                    print $(i + 1);
                    exit;
                }
            }
        }
    ' "$CMAKE_FILE")"

    if [[ -z "$PROJECT_VERSION" ]]; then
        record_error "Failed to extract project VERSION from CMakeLists.txt."
    elif ! [[ "$PROJECT_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]]; then
        record_error "Project version '${PROJECT_VERSION}' does not follow Semantic Versioning (e.g. 1.0.0 or 0.2.0-rc.1)."
    else
        log_ok "Detected project version: ${PROJECT_VERSION} in CMakeLists.txt."
    fi
fi

# 6. Check tag existence
TAG_NAME="v${PROJECT_VERSION}"
if [[ -n "$PROJECT_VERSION" ]]; then
    if git rev-parse -q --verify "refs/tags/${TAG_NAME}" >/dev/null 2>&1; then
        EXISTING_SHA="$(git rev-parse --short "refs/tags/${TAG_NAME}")"
        record_error "Tag '${TAG_NAME}' already exists locally (pointing to commit ${EXISTING_SHA})."
    else
        log_ok "Tag '${TAG_NAME}' does not exist locally."
    fi

    if [[ "$CHECK_REMOTE" == true ]] && git remote | grep -q "^origin$"; then
        if git ls-remote --tags origin "refs/tags/${TAG_NAME}" 2>/dev/null | grep -q "${TAG_NAME}"; then
            record_error "Tag '${TAG_NAME}' already exists on remote 'origin'."
        else
            log_ok "Tag '${TAG_NAME}' does not exist on remote 'origin'."
        fi
    fi
fi

# 7. Check Changelog.md
CHANGELOG_FILE="$REPO_ROOT/Changelog.md"
if [[ ! -f "$CHANGELOG_FILE" ]]; then
    record_error "Changelog.md not found at repository root."
elif [[ -n "$PROJECT_VERSION" ]]; then
    FOUND=0
    HEADER=""
    VER_NOTES=0
    UNRELEASED_NOTES=0

    while IFS='=' read -r key val; do
        case "$key" in
            FOUND) FOUND="$val" ;;
            HEADER) HEADER="$val" ;;
            VER_NOTES) VER_NOTES="$val" ;;
            UNRELEASED_NOTES) UNRELEASED_NOTES="$val" ;;
        esac
    done < <(awk -v ver="$PROJECT_VERSION" '
        BEGIN {
            found_ver = 0;
            header = "";
            ver_notes = 0;
            in_ver = 0;
            in_unreleased = 0;
            unreleased_notes = 0;
        }
        /^##[[:space:]]+Unreleased/ {
            in_unreleased = 1;
            next;
        }
        $0 ~ "^##[[:space:]]+" ver "([[:space:]]|$)" {
            in_unreleased = 0;
            in_ver = 1;
            found_ver = 1;
            header = $0;
            next;
        }
        /^##[[:space:]]+/ {
            in_ver = 0;
            in_unreleased = 0;
        }
        in_unreleased {
            if ($0 ~ /[^[:space:]]/) unreleased_notes++;
        }
        in_ver {
            if ($0 ~ /[^[:space:]]/) ver_notes++;
        }
        END {
            print "FOUND=" found_ver;
            print "HEADER=" header;
            print "VER_NOTES=" ver_notes;
            print "UNRELEASED_NOTES=" unreleased_notes;
        }
    ' "$CHANGELOG_FILE")

    if [[ "$FOUND" -eq 0 ]]; then
        record_error "No section for version '${PROJECT_VERSION}' found in Changelog.md (expected '## ${PROJECT_VERSION} YYYY-MM-DD')."
    else
        log_ok "Found changelog section: '${HEADER}'."

        if [[ "$VER_NOTES" -eq 0 ]]; then
            record_error "Changelog section for '${PROJECT_VERSION}' contains no release notes."
        else
            log_ok "Changelog section has ${VER_NOTES} line(s) of release notes."
        fi

        # Check date in changelog header
        HEADER_DATE="$(echo "$HEADER" | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}' || true)"
        TODAY_DATE="$(date +%Y-%m-%d)"
        if [[ -z "$HEADER_DATE" ]]; then
            log_warn "Changelog section header does not include a release date in YYYY-MM-DD format."
        elif [[ "$HEADER_DATE" != "$TODAY_DATE" ]]; then
            log_warn "Changelog date is '${HEADER_DATE}', but today's date is '${TODAY_DATE}'."
        else
            log_ok "Changelog release date matches today (${TODAY_DATE})."
        fi
    fi

    if [[ "${UNRELEASED_NOTES:-0}" -gt 0 ]]; then
        log_warn "The '## Unreleased' section in Changelog.md still contains ${UNRELEASED_NOTES} line(s). Ensure all changes for this release were moved."
    else
        log_ok "The '## Unreleased' section is empty."
    fi
fi

# 8. Check result evaluation
echo ""
if [[ "$ERRORS_FOUND" -gt 0 ]]; then
    if [[ "$FORCE" == true ]]; then
        log_warn "Proceeding despite ${ERRORS_FOUND} check failure(s) due to --force."
    else
        log_fail "Pre-release checks failed with ${ERRORS_FOUND} error(s). Tagging aborted."
        exit 1
    fi
else
    log_ok "${COLOR_BOLD}All pre-release checks passed successfully!${COLOR_RESET}"
fi

# 9. Perform tagging / dry-run
TAG_MESSAGE="${CUSTOM_MESSAGE:-StripeKit ${PROJECT_VERSION}}"
HEAD_COMMIT="$(git rev-parse --short HEAD)"

if [[ "$DRY_RUN" == true ]]; then
    echo ""
    log_info "${COLOR_BOLD}[DRY RUN]${COLOR_RESET} Release would be tagged as follows:"
    echo "  Tag:     ${TAG_NAME}"
    echo "  Commit:  ${HEAD_COMMIT}"
    echo "  Message: ${TAG_MESSAGE}"
    if [[ "$PUSH_TAG" == true ]]; then
        echo "  Push:    git push origin ${TAG_NAME}"
    else
        echo "  Push:    (Tag would not be pushed automatically; use --push to enable)"
    fi
    exit 0
fi

echo ""
log_info "Creating annotated release tag '${TAG_NAME}' on commit ${HEAD_COMMIT}..."
git tag -a "${TAG_NAME}" -m "${TAG_MESSAGE}"
log_ok "Successfully created tag '${TAG_NAME}'."

echo ""
git show --no-patch "${TAG_NAME}"
echo ""

if [[ "$PUSH_TAG" == true ]]; then
    log_info "Pushing tag '${TAG_NAME}' to remote 'origin'..."
    git push origin "${TAG_NAME}"
    log_ok "Successfully pushed tag '${TAG_NAME}' to origin."
else
    log_info "To push this release tag to the remote repository, run:"
    echo -e "  ${COLOR_BOLD}git push origin ${TAG_NAME}${COLOR_RESET}"
fi
