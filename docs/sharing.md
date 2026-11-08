# Share timestamps

[← Documentation](../README.md#table-of-contents)

Sharing is optional. Review timestamps against your copy of an episode before
sending them to [IntroDB](https://introdb.app) or [TheIntroDB](https://theintrodb.org).
These are separate community services with separate API keys.

## Configure a service

1. Obtain an API key from the service you want to use.
2. Open **Settings → Endpoints**.
3. Enable that service and enter its **API Key**. Keep the default URLs unless
   you intentionally use a different endpoint.

IntroDB uses the show's IMDb ID. TheIntroDB uses TVDB identification, with
supported TMDB or IMDb mapping. For local media, [fetch metadata or identify
the show](setup.md#local-show-titles-and-artwork) first. Correct episode numbering
is especially important for specials and alternative episode orders.

## Submit selected timestamps

1. Open a show and season.
2. Select the segment types beside the episodes you want to share. The type
   switches above the list select that type across the current season.
3. Choose **introdb.app** or **theintrodb.org** from the service selector.
4. Click **Submit**.
5. Check the show, season, IDs, segment types, and counts in **Review Submission**.
   For TheIntroDB, also check the mapped TVDB episodes.
6. Click **Confirm** to add the submission to the queue.

Repeat for other seasons or services as needed. Detection alone does not submit
anything, and submissions do not update your media server's markers.

For a movie, select timestamps on its movie card. Movie editing allows segments
of at least 5 seconds, with a maximum of 5 minutes for Intro, 20 minutes for Recap
and 30 minutes for Credits or Preview. TheIntroDB accepts those lengths.
IntroDB accepts movie Credits up to 15 minutes from INTROvert.

## Queue and history

**Queue** shows pending and active detection/submission work. New submissions
can be added while earlier ones are processing. **History** shows completed
work and submission totals. Use search or click a column heading to sort;
click a show title to return to it.

The submission queue is saved across restarts. Detection scans must be started
again after quitting.

Episode feedback includes **Submitted**, **Exists**, **Queued**, **Submitting**,
**Limited**, **Skipped**, and **Failed**. **Exists** means the service already
has a matching timestamp. Click **FAILED** for details on an unsuccessful submission.

INTROvert checks for duplicates and observes service rate limits. If a service
limits requests, let the queue wait rather than repeatedly submitting the same
items. For failures, correct the reported key, identification, or connection
problem before reviewing and submitting again.
