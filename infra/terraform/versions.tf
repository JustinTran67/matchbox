terraform {
  required_version = ">= 1.5"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.70"
    }
  }
}

provider "aws" {
  region = var.region

  default_tags {
    tags = {
      Project = "matchbox"
      # Everything this stack creates is meant to be destroyed the same session it is
      # created. The tag makes an orphaned resource obvious in the console.
      Ephemeral = "true"
    }
  }
}
