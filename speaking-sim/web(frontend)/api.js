// The browser's side of the JSON API: one reader for every /api route, because
// every one of them answers JSON, errors included.
//
// It replaced twelve hand-rolled fetch calls spread over six files, each
// repeating credentials: "same-origin", the .json() that has to tolerate a body
// that is not JSON, and its own idea of what a failure looks like. The three
// shapes they had between them are the two functions below.
//
// Nothing here knows about any particular route. The static files the listening
// page reads are fetched directly, not through this: a manifest is not an API
// call and has no .error to read.

function request(method, path, body) {
    const options = { method, credentials: "same-origin", headers: {} };
    if (body !== undefined) {
        options.headers["Content-Type"] = "application/json";
        options.body = JSON.stringify(body);
    }
    return fetch(path, options);
}

// Resolves to the parsed body. Rejects with an Error carrying `status` when the
// server refused, and with no `status` when the request never arrived - which is
// the difference between "the API said no" and "there is no API", and some
// callers say different things about the two.
export function api(method, path, body, refusalMessage) {
    return request(method, path, body).then((response) =>
        response
            .json()
            .catch(() => ({}))
            //a refusal is not obliged to carry a body, and a 500 from a crashed
            //handler may carry one that is not JSON at all
            .then((data) => {
                if (!response.ok) {
                    const failure = new Error(data.error || refusalMessage ||
                        `request failed (${response.status})`);
                    failure.status = response.status;
                    throw failure;
                }
                return data;
            }));
}

// Resolves to the parsed body, or to `fallback` for any failure at all. For the
// reads a page can do without: no class list is still a working exam page, and
// an empty language list still lets the server pick its own default.
export function apiOr(fallback, path) {
    return request("GET", path)
        .then((response) => (response.ok ? response.json() : fallback))
        .catch(() => fallback);
}
