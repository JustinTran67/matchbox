#!/usr/bin/env bash
# Brings up the whole stack on a local kind cluster and verifies it end to end.
# This is the exact sequence the README's results were produced with.
set -euo pipefail

CLUSTER="${CLUSTER:-matchbox}"
IMAGE="${IMAGE:-matchbox-engine:dev}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"

echo "==> creating kind cluster '${CLUSTER}' (skipped if it exists)"
kind get clusters | grep -qx "${CLUSTER}" || kind create cluster --name "${CLUSTER}"

echo "==> building the service image"
docker build -t "${IMAGE}" -f "${ROOT}/service/Dockerfile" "${ROOT}"

# kind nodes have their own image store, so a locally built image has to be side-loaded;
# there is no registry in this setup.
echo "==> loading the image into the cluster"
kind load docker-image "${IMAGE}" --name "${CLUSTER}"

echo "==> deploying kafka"
kubectl apply -f "${HERE}/kafka.yaml"
kubectl wait --for=condition=ready pod/kafka-0 --timeout=300s

echo "==> creating topics"
kubectl apply -f "${HERE}/kafka-topics-job.yaml"
kubectl wait --for=condition=complete job/kafka-topics --timeout=300s

echo "==> deploying the engine StatefulSet"
kubectl apply -f "${HERE}/engine.yaml"
kubectl wait --for=condition=ready pod -l app=matchbox-engine --timeout=300s

echo "==> partition ownership as claimed by each pod"
for i in 0 1 2 3; do
  kubectl logs "matchbox-engine-${i}" | grep -E 'partitions=' || true
done

echo "==> running end-to-end verification"
kubectl delete pod matchbox-e2e --ignore-not-found >/dev/null
kubectl run matchbox-e2e \
  --image="${IMAGE}" --image-pull-policy=IfNotPresent --restart=Never \
  --env="KAFKA_BROKERS=kafka-0.kafka.default.svc.cluster.local:9092" \
  --env="STEPS=${STEPS:-5000}" --env="WAIT_SECONDS=120" \
  --command -- /usr/local/bin/matchbox_e2e >/dev/null

kubectl wait --for=jsonpath='{.status.phase}'=Succeeded pod/matchbox-e2e --timeout=600s \
  || kubectl wait --for=jsonpath='{.status.phase}'=Failed pod/matchbox-e2e --timeout=10s
kubectl logs matchbox-e2e

exit_code="$(kubectl get pod matchbox-e2e -o jsonpath='{.status.containerStatuses[0].state.terminated.exitCode}')"
echo "==> verification exit code: ${exit_code}"
echo "==> tear down with: kind delete cluster --name ${CLUSTER}"
exit "${exit_code}"
