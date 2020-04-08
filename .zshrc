HISTFILE=$HOME/.zsh_history
HISTSIZE=20000
SAVEHIST=20000

export LANG=en_US.UTF-8

# Compilation flags
export ARCHFLAGS="-arch x86_64"

export EDITOR=nano
export PAGER=less
export LIBGL_DRI3_ENABLE=1
export XDG_RUNTIME_DIR=/tmp

# modules
autoload -U compinit promptinit
compinit
promptinit

# mostly just right (for me) prompt
prompt bart

# options
setopt autocd
setopt completealiases
setopt extended_glob
setopt interactive_comments
setopt inc_append_history
setopt share_history
setopt hist_ignore_all_dups
setopt hist_ignore_space
setopt hist_reduce_blanks

# rehash completion files automatically
zstyle ':completion:*' rehash true

# keybindings 
bindkey "\e[3~" delete-char
bindkey "^[[H" beginning-of-line
bindkey "^[[F" end-of-line
bindkey "^[[A"  history-beginning-search-backward
bindkey "^[[B"  history-beginning-search-forward
bindkey '^[[1;5D'   backward-word
bindkey '^[[1;5C'   forward-word


# aliases
alias ping='ping -c 3'
alias g='grep'
alias se="sudo $EDITOR"
