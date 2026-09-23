// The signed-in-as corner of the nav, shared by every page.
//
// Kept separate from client.js because the listening page needs it too and has
// none of the exam machinery. Like translate.js it defines one global and is
// loaded with a plain <script src>, so load order is the only dependency.

function initAccount() {
    const nav = document.getElementById("siteNav");
    if (!nav) return;

    const box = document.createElement("span");
    box.id = "accountBox";
    nav.appendChild(box);

    fetch("/api/me", { credentials: "same-origin" })
        .then((response) => (response.ok ? response.json() : null))
        .then((user) => paintAccount(box, user))
        .catch(() => paintAccount(box, null));
    // a failed fetch is painted as signed out rather than left blank: the
    // server being unreachable and nobody being signed in look the same from
    // here, and both mean the same thing to the person reading the nav
}

function paintAccount(box, user) {
    box.textContent = "";

    if (!user) {
        const link = document.createElement("a");
        link.href = "/signin";
        link.textContent = "Sign in";
        box.appendChild(link);
        return;
    }

    const name = document.createElement("span");
    name.id = "accountName";
    name.textContent = user.name || user.email;
    box.appendChild(name);

    const form = document.createElement("form");
    form.method = "POST";
    form.action = "/auth/logout";
    // a form rather than a link: /auth/logout is POST only, so a prefetch or a
    // crawler cannot sign somebody out by following it

    const button = document.createElement("button");
    button.type = "submit";
    button.id = "signOutButton";
    button.textContent = "Sign out";
    form.appendChild(button);
    box.appendChild(form);
}
